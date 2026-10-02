#pragma once
// The pedestrians (pool 2) drawn with their own
// models, posed by the PORTED animation machine.
//
// Which model: DATA\PED01A.GEO's two RMD3s, ids 400 / 430 (the registry record at +0x60, which the ported
// ModelBind picked). The pose: the object's 17 part slots (+4), which the PORTED ApplyFrame writes; the
// model draw walks them with RiderParts 0x8006745C's program (kind 4 takes the riders' arm, model_draw.h),
// so the posing is lod_pose_draw.h's with LodOwner::Rider. The sheet: ModelKeySet SLUS 0x800302C4 binds the
// object to record +0x4A of the sheet table *(0x8005B2E4), whose key is a kind-4 LECT of the race's level
// bundle; the TIM of that LECT with its own CLUT (RASHCDI 0x8005DDB8 uploads that CLUT at the record's
// +10). Where: the object's part-0 matrix +0x68 = the rows +0x1B0 transposed (0x8008D56C) at the position
// +0xB8. OURS, named: LOD 0 is drawn whatever +0x08 holds (the pedestrians' LOD choice is not ported), and
// the root triple +0x1C is added along the rows as for a rider (rider_pose_draw.h).
#include "render/gl_api.h"
#include "render/gpu_texture.h"
#include "render/mat4.h"
#include "rrformats/pose.h"
#include "rrformats/rmd3.h"
#include "rrvfs/disc_image.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace rr::render {

class RaceScene;
struct LodPoseMesh;
struct CapturedVerts;

struct PedInstance {
    uint32_t model = 0;       // 400 / 430
    uint32_t sheet = 0;       // the LECT id of its sheet record, 0 = none
    int32_t pos[3] = {};      // 16.16 world
    int16_t rows[9] = {};     // +0x1B0: row k = model axis k in the world (4.12)
    int16_t root[3] = {};     // +0x1C, model units
    rr::PartMatrix slots[17]; // +4's part slots
    int64_t flags = -1;       // +0x24, for the model light (race_scene.h ApplyObjectLight); -1 off
    uint32_t entity = 0;      // the object (its cell's ordering table, race_scene_ot.cpp)
    int lod = 0;              // +0x08, the LOD the PORTED ModelVisible chose (0 when the model pass does not run it)
    const CapturedVerts* captured = nullptr; // its vertices as the PORTED model draw left them this view
};

class PedDraw {
public:
    // Parses DATA\PED01A.GEO and the level bundle's LECT sheets; a GL context must be current.
    bool Load(const rr::DiscImage& disc, int raceId);
    size_t Draw(const RaceScene& scene, const Mat4& viewProj, const std::vector<PedInstance>& peds);
    size_t Textured() const { return textured_; }
    size_t Unknown() const { return unknown_; }

private:
    const GpuIndexedTexture* Sheet(uint32_t id);
    std::map<uint32_t, std::vector<std::shared_ptr<LodPoseMesh>>> meshes_; // model id -> LOD k (every group)
    std::map<uint32_t, std::vector<rr::ModelGroup>> groups_;               // model id -> group k (GteObjectSoup)
    std::vector<rr::TriangleSoup::Vertex> gteSoup_;
    std::map<uint32_t, uint32_t> modelSheet_;                 // model id -> DOD3 +0x1C
    std::map<uint32_t, std::pair<bool, int>> modelLight_;     // model id -> (normals present, DOD3 class): the model light
    std::map<uint32_t, std::vector<uint8_t>> tims_;           // LECT id -> its TIM
    std::map<uint32_t, GpuIndexedTexture> sheets_;
    GLuint vao_ = 0, vbo_ = 0;
    size_t textured_ = 0, unknown_ = 0;
};

} // namespace rr::render
