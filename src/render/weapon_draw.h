#pragma once
// The weapon in a rider's hand, drawn from the arena
// the PORTED weapon code keeps.
//
// What is the original's: which rider holds a weapon object - seat 0 of the rider, +0x38, a model object
// of 0x800CF018 written by the ported Attach inside WeaponObject RASHCDG 0x800958F0; which group of model
// 800 it shows - the object's LOD +0x08, which LodSelect set to the weapon in hand; and WHERE it is - the
// seat's kind +0x3C (7 or 10) names the attachment program's control word (RASHCDG 0x80067064: a word
// with code 3 whose bits 18..22 equal the kind copies the matrix-stack entry of the part just placed to
// the object's +0x68..+0x87). In the 17-part rider program (0x800CC790 words 7 and 11) those words follow
// parts 7 and 10, the ends of the two arms, so the weapon takes that part's world rotation and origin,
// which PoseGroup computes the same way the rider's own mesh is posed (rider_pose_draw.h).
//
// Its parts: the object's own part slots (+0x04, 24 bytes each, the 3x3 at +4) as the PORTED animation
// pass leaves them - the nunchaku and the chain (4 parts, program 6) have an animation object on the
// weapons bank (WeaponObject's ViewSlot), whose Pose writes those slots every frame; a one-part weapon's
// slot 0 is its rest rotation. PoseGroup walks them as 0x80067064 does.
//
// Its texture: the sheet is LECT <DOD3+0x1C> (49) of BBLEVEL<bank+1>.TEX, 4bpp 256 x 32 (VRAM (960, 224) in
// rr-race), and the 16-colour palette primitive p selects (its `tpage` field, 0x800251E4's CLUT index) is
// the one the emitter's 40-entry loop names from the object's model key record (+0x4A: rr-race's record
// 0, key 49, CLUT spec 0x0FFE, x 0x3C0): VRAM (960 + 16 (p & 3), 255 - (p >> 2)) - the sheet's OWN last
// rows (model_texture.h BuildLectAtlas). OURS, named: the key record is not built in the product (no
// texture loader), so the renderer applies that record's rule for the model's sheet directly.
#include "render/gl_api.h"
#include "render/gpu_texture.h"
#include "render/mat4.h"
#include "rrformats/pose.h"
#include "rrformats/rmd3.h"
#include "rrformats/skeleton.h"
#include "rrvfs/disc_image.h"

#include <cstdint>
#include <vector>

namespace rr::render {

class RaceScene;

// VR (tools\rrgame\vr_handlebars.h): the Handlebars mode hides the player's rider in the head view and
// draws the player's own tracked hands, so the player's weapon goes into the tracked hand that holds it. `origin`: the
// fist's centre (world); `along`: the world direction the weapon points out of the fist - the weapon's own long axis
// (its farthest vertex from its attach point, the hand part's origin) is turned onto it; `side`: a world direction
// across it; `scale`: world units per model unit. `hidden`: the player's weapon is not drawn (no tracked hand). Only
// rider `rider`; inactive (the default, every desktop run): the rider's own posed hand.
struct WeaponHandOverride {
    bool active = false, hidden = false;
    uint32_t rider = 0;
    float origin[3] = {}, along[3] = {0, -1, 0}, side[3] = {1, 0, 0};
    float scale = 1.0f / 1024.0f;
    // VR physical combat (tools\rrgame\vr_melee.h): the weapon in hand (model 800 group = riderDef +0x2E)
    // drawn at rest in the tracked hand while the rider holds no weapon OBJECT yet (the original seats one only when a
    // fight stance starts) - the player sees, and hits with, what he holds; -1: only an object is drawn.
    int unheld = -1;
    // VR (tools\rrgame\vr_nunchaku.h): a jointed weapon (nunchaku, chain) in the tracked hand follows a
    // simulated chain: `chainCount` world points - the handle's far end first, then the far end of each next part - used
    // when it equals the weapon group's part count. Part 0 stays as the hand holds it; each next part is turned (the
    // smallest turn, carried down the chain; ChainSlots) from its rest direction onto its simulated segment in the
    // weapon's model frame; the hand frame is the REST pose's long axis (the handle stays in the fist). 0: the object's
    // own slots.
    int chainCount = 0;
    float chain[4][3] = {};
    // VR (tools\rrgame\vr_weapon_calib.h): the weapon's calibrated grip - its whole model matrix (model units
    // -> world) given; origin / along / side are then unused. False: the long axis onto `along`.
    bool useMatrix = false;
    Mat4 matrix;
};
// VR (tools\rrgame\vr_holsters.h): the weapons on the player's hips - model 800 group `group` at rest with its
// own model matrix; drawn by WeaponDraw::DrawExtras. Empty (the default, every desktop run): nothing.
struct ExtraWeaponDraw {
    int group = -1;
    Mat4 matrix;
};
void SetExtraWeapons(const std::vector<ExtraWeaponDraw>& list);
void SetWeaponHandOverride(const WeaponHandOverride& o);
const WeaponHandOverride& CurrentWeaponHandOverride(); // the last one set (VR physical combat adds `unheld`)
// The weapon soup's model matrix in a tracked hand: its long axis (the farthest vertex of `soup`) onto o.along, o.side
// across it, at o.origin, o.scale world units per model unit (what Draw sets for a weapon in a tracked hand).
Mat4 WeaponHandMatrix(const rr::TriangleSoup& soup, const WeaponHandOverride& o);
// A chain group's rest shape (the nunchaku, the chain: 4 parts, each hung on the far end of the previous
// by its attachment link - the link vertex of the parent part; the last part to the far end of its vertex extent's
// long axis): per part its rest direction (part-local, unit) and length (model units). False: not such a chain.
struct ChainShape {
    int parts = 0;
    float axis[4][3] = {};
    float length[4] = {};
};
bool ChainShapeOf(const rr::ModelGroup& group, const rr::SkeletonTable& skeleton, const rr::Assembly& assembly, ChainShape& out);
// The parts' LOCAL slots (parent^T x world, the attachment walk's order) that turn each part k >= 1 onto the world
// segment chain[k - 1] -> chain[k] under the weapon's model matrix `model` (its rotation; any uniform scale).
void ChainSlots(const Mat4& model, const ChainShape& shape, const float chain[4][3], rr::PartMatrix out[4]);

class WeaponDraw {
public:
    // BBLEVEL<bank+1>.GEO: model 800's groups and the rider model 150's group 0 (the hands), and the sheet
    // out of BBLEVEL<bank+1>.TEX; a GL context must be current. False when a file or a model is missing.
    bool Load(const rr::DiscImage& disc, int bank);
    // Draws the weapon rider `rider` (a pool-1 address in the 2 MiB guest image `ram`) holds, in the rider
    // object's frame `riderModel` with its posed part slots `riderLocal` (17, nullptr = rest). False when
    // the rider holds none.
    bool Draw(const RaceScene& scene, const Mat4& viewProj, const uint8_t* ram, uint32_t rider, const Mat4& riderModel,
              const rr::PartMatrix* riderLocal);
    // VR: the weapons SetExtraWeapons listed (the holsters).
    void DrawExtras(const RaceScene& scene, const Mat4& viewProj);
    size_t ExtrasTotal() const { return extrasDrawn_; }
    size_t DrawnTotal() const { return drawn_; }
    size_t PosedTotal() const { return posed_; }   // draws whose parts were not all at rest
    bool Textured() const { return sheet_.Valid(); }

private:
    struct Group {
        rr::ModelGroup group;
        rr::Assembly assembly;
    };
    std::vector<Group> groups_; // model 800 group k (weapon k)
    rr::ModelGroup riderGroup_;
    rr::SkeletonTable skeleton_;
    rr::Assembly riderAssembly_;
    bool haveRider_ = false;
    GpuIndexedTexture sheet_;
    GLuint vao_ = 0, vbo_ = 0;
    size_t drawn_ = 0, posed_ = 0, extrasDrawn_ = 0;
};

} // namespace rr::render
