// Player 2's own sidecar rig (race_scene.h LoadSidecarFor).
//
// LoadBikeBank RASHCDI 0x8005C45C (RASHCDI SHA-1 9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06) walks EVERY player
// (game_state+0x30) and loads DATA\<name>.MRO for each one whose bike index game_state+0x48 + 4p is a sidecar
// (0x8005C570..0x8005C584); SpawnBike 0x80065A94 gives each player's bike its own palette a1 = 0x8005DD9C(p) (TSLP
// block 6u / 6u + 3 of that player, u = p in a two-player race; jail_session.h PlayerPaletteBlock). So in a two-player
// Side Car race (type 0x18) each player has a rig of their own: the scene keeps a second
// rig, loaded by the same LoadSidecar with player 2's index and palette block, and swaps it into the rig members to
// load and to draw it (DrawRequest::sidecarRig = 1). The swap covers every member LoadSidecar / DrawSidecar own
// (race_scene.h, `sidecar*_`); the run counters stay shared.
#include "render/race_scene.h"

#include "render/lod_pose_draw.h"
#include "render/rider_pose_draw.h"

#include <utility>

namespace rr::render {

struct SidecarRigSlot {
    GLuint vao = 0, vbo = 0;
    std::vector<ModelPart> parts;
    std::vector<GpuIndexedTexture> sheets;
    std::shared_ptr<RiderPoseMesh> pose;
    size_t poseParts = 0;
    float attachLod[4][3] = {};
    uint32_t model = 0;
    std::shared_ptr<LodPoseMesh> lod[4];
    bool lodDirty[4] = {};
};

void RaceScene::SwapSidecarRig() {
    if (!sidecarRig2_) sidecarRig2_ = std::make_shared<SidecarRigSlot>();
    SidecarRigSlot& s = *sidecarRig2_;
    std::swap(sidecarVao_, s.vao);
    std::swap(sidecarVbo_, s.vbo);
    std::swap(sidecarParts_, s.parts);
    std::swap(sidecarSheets_, s.sheets);
    std::swap(sidecarPose_, s.pose);
    std::swap(sidecarPoseParts_, s.poseParts);
    for (int l = 0; l < 4; ++l) {
        for (int k = 0; k < 3; ++k) std::swap(sidecarAttachLod_[l][k], s.attachLod[l][k]);
        std::swap(sidecarLod_[l], s.lod[l]);
        std::swap(sidecarLodDirty_[l], s.lodDirty[l]);
    }
    std::swap(sidecarModel_, s.model);
}

bool RaceScene::LoadSidecarFor(int rig, const rr::DiscImage& disc, uint32_t bikeIndex, int bundle, int paletteBlock) {
    if (rig == 0) return LoadSidecar(disc, bikeIndex, bundle, paletteBlock);
    SwapSidecarRig(); // player 1's rig aside, rig 1's (empty) in
    const bool ok = LoadSidecar(disc, bikeIndex, bundle, paletteBlock);
    SwapSidecarRig(); // rig 1 aside again, player 1's back
    return ok;
}

bool RaceScene::HasSidecarFor(int rig) const {
    if (rig == 0) return HasSidecar();
    return sidecarRig2_ && !sidecarRig2_->parts.empty();
}

uint32_t RaceScene::SidecarModelIdFor(int rig) const {
    if (rig == 0) return SidecarModelId();
    return sidecarRig2_ ? sidecarRig2_->model : 0u;
}

size_t RaceScene::SidecarPartsFor(int rig) const {
    if (rig == 0) return SidecarParts();
    return sidecarRig2_ && sidecarRig2_->pose ? sidecarRig2_->poseParts : 0u;
}

const float* RaceScene::SidecarAttachFor(int rig, int lod) const {
    if (rig == 0 || !sidecarRig2_) return SidecarAttach(lod);
    return (lod > 0 && lod < 4) ? sidecarRig2_->attachLod[lod] : sidecarRig2_->attachLod[0];
}

} // namespace rr::render
