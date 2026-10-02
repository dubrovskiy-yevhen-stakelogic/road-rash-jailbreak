#pragma once
// VR nunchaku - OURS: the jointed weapons swing on a physics chain in the tracked hand.
//
// The weapons: the NUNCHAKU is weapon 4 and the CHAIN weapon 0 - the rap sheet RASHCDF 0x80074554
// (PORTED) draws owned bit 0 as CHAN and bit 4 as NCHK, the ported HUD's icon (art 41 + weapon) is a chain for 0 and a
// nunchaku for 4, and the models agree (docs\formats\rules.md). Both are model 800 groups of
// BBLEVEL1.GEO, four parts each, attachment program 6: part 0 is carried at the object's origin (the fist) and each
// next part hangs on the far end of the previous one - the link vertex of the parent part (read at load, logged;
// weapon_draw.h ChainShapeOf): group 4, the nunchaku, a 277 stick, two links of 101 / 97 and a 419 stick; group 0,
// the chain, four equal links 177 / 180 / 184 / 187 (model units, 1024 a world unit) - each part with its own matrix
// slot (1, 2, 3). In the race the original swings parts 1..3 with a clip of
// ANIMTBLW (the object's animation object, weapon_draw.h); in the VR hand that clip is not the player's motion, so
// there the parts follow a chain simulated here instead:
//   * part 0 (the handle in the fist) rides the hand exactly as the weapon is drawn - the model matrix of the FINAL
//     hand override (weapon_draw.h WeaponHandMatrix: the rest pose's long axis on `along`, or a calibrated grip's own
//     matrix). The step runs before the blows' sweep, with this frame's hand pose times the
//     hand-to-weapon relation observed on the override the weapon was drawn with last frame (Observe, from
//     vr_melee.cpp Draw) - a constant grip calibration costs nothing; the handle's far end is the chain's fixed point;
//   * the far ends of parts 1, 2, 3 are point masses (hand-relative Verlet, 4 sub-steps a frame) held at the model's own
//     part lengths (four constraint passes), pulled by gravity (the seat's down, 9.81 m/s^2), damped (1.5 / s) and
//     pushed by the hand's own acceleration (clamped at 600 m/s^2: beyond, the anchor jumped - a teleport, a tracking
//     jump - and the chain keeps its shape against the hand);
//   * drawn: the joints go into the tracked hand's weapon override (weapon_draw.h chainCount / chain), where each part
//     is turned by the smallest turn onto its segment under the matrix the weapon is drawn with (ChainSlots) - the
//     model's own vertices, the model's own joints;
//   * the hit collider (vr_melee.h): the swinging end - the last part, from its joint to its tip - so a blow is the end
//     striking, its speed the end's own (the sweep's contact speed); the handle does not hit.
// Settings: [vr] melee_nunchaku=1 (Combat options "Nunchaku / chain physics"); off: the weapon is drawn by the
// original's clip (the object's own slots) and its collider is the straight capsule (vr_melee.h).
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "render/weapon_draw.h"
#include "rrformats/pose.h"
#include "rrformats/rmd3.h"
#include "rrformats/skeleton.h"

namespace rrgame {

class VrNunchaku {
public:
    // Model 800 groups 0 and 4: their chain shapes and rest soups.
    bool Load(const std::vector<rr::Model>& models, const rr::SkeletonTable& skeleton);
    bool Jointed(int weapon) const; // a weapon this simulates (4 nunchaku, 0 chain)
    // Once per VR frame for the hand holding weapon `weapon`: the tracked hand's grip pose in the world (`pos`, its
    // `right` / `up` / `fwd` axes), `down` the world's down (unit), `upm` world units per metre. Not jointed, or `valid`
    // false: the chain is reset (it starts hanging on from the handle the next frame it is held).
    void Update(int weapon, bool valid, const float pos[3], const float right[3], const float up[3], const float fwd[3],
                const float down[3], float dt, float upm);
    // The override the weapon was drawn with this frame (after every VR layer set it): the hand-to-weapon relation for
    // the next frame's step.
    void Observe(const rr::render::WeaponHandOverride& o);
    bool Active() const { return active_; }
    // The swinging end (the last part, joint to tip), world; false when not active.
    bool Collider(float root[3], float tip[3]) const;
    // The chain into the tracked hand's override (weapon_draw.h chainCount / chain); false when not active.
    bool Fill(rr::render::WeaponHandOverride& o) const;
    // The chain's joints (the fist, the handle's end, then each part's far end) for the debug lines, world.
    std::vector<std::array<float, 3>> Joints() const;
    std::string Totals() const;

private:
    struct Model {
        bool ok = false;
        rr::render::ChainShape shape;
        rr::TriangleSoup restSoup; // the hand frame (WeaponHandMatrix)
    } models_[2];                  // [0] the chain (group 0, weapon 0), [1] the nunchaku (group 4, weapon 4)
    const Model* model_ = nullptr;
    int weapon_ = -1;
    bool active_ = false;
    float scale_ = 1.0f / 1024.0f;          // world units per model unit
    float relRot_[9] = {}, relPos_[3] = {}; // the weapon's model matrix in the hand's frame (rotation x scale, origin)
    bool haveRel_ = false;
    int relWeapon_ = -1;
    float handRot_[9] = {}, handPos_[3] = {}; // this frame's hand frame (columns right, up, back), world
    bool haveHandFrame_ = false;
    std::array<float, 3> hand_{}, handVel_{};
    bool haveHand_ = false;
    std::array<float, 3> j_[5] = {};        // j0 the fist, j1 the handle's end (fixed to the hand), j2..j4 simulated
    std::array<float, 3> prev_[5] = {};     // the simulated ones' last sub-step, against j1
    struct Totals {
        size_t frames = 0, resets = 0, clamps = 0, observed = 0;
        float maxTip = 0.0f, maxAngle = 0.0f; // the end's fastest speed against the hand (m/s), the widest bend (deg)
        float maxDrop = 0.0f, chainLength = 0.0f; // the tip's deepest below the handle's end along gravity, the chain (m)
    } totals_;
};

} // namespace rrgame
