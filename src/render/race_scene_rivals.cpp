// The other riders' own machines (race_scene.h LoadRivalMachines / DrawRivalMachine).
//
// Every rival has a machine of its own, not the PLAYER's (bike model 100 and rider model 150 in the player's palettes):
// the whole-frame comparison against the original's packets (tools\scout\psxgpu.py parity) shows the rival beside the
// player as a red and black bike with a dark rider. What the original draws, per object:
//   * the MODEL is the object's own registration: *(obj + 0x60) -> the registered model, whose first word is the
//     model id (ModelBind; the product's arena holds the same ids the captures do - race 1/4: bikes 100 / 109
//     alternating, riders 150 / 159, the police bikes 118);
//   * the PALETTE is the object's own a1 = (obj + 0x24) bits 12..17: CLUT
//     (640 + (a1 % 3) * 128, 511 - a1 / 3). Measured against the quick capture's VRAM, byte for byte in
//     BBLEVEL1.TEX: a1 0..13 are KNBP block 45 - 3 (a1 / 3) + a1 % 3 (the loader's positional upload), a1 29 is
//     KNBP block 46 and a1 30 / 31 are TSLP blocks 1 / 2 (the runtime copies the player's rows get). The riders
//     and bikes SpawnBike seeds in race 1/4 use a1 0..13 only.
//   * the wheels (prim.clut bit 7) take RIMA1.TIM under the bike's palette (race_scene.cpp LoadMachine's rule).
// The geometry, part posing (LOD 0 from the part slots, LODs 1..3 through lod_pose_draw.h) and the seat per LOD
// (SeatVertex RASHCDG 0x80066A84 / ChildPlace 0x80066B98) are LoadMachine's rules applied to the other models.
// NOT reproduced for them: the shadow hull (DrawShadow keeps model 100's quadsD).
#include "render/race_scene.h"

#include "render/lod_pose_draw.h"
#include "render/rider_pose_draw.h"
#include "rrformats/model_texture.h"
#include "rrformats/rmd3.h"
#include "rrformats/skeleton.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <exception>

namespace rr::render {

struct RivalMachine {
    uint32_t bikeId = 0, riderId = 0;
    uint16_t bikeTex = 0, riderTex = 0;
    GLuint vao = 0, vbo = 0;
    std::vector<ModelPart> parts; // sheet: 0 the bike's LECT, 1 the rim, 2 the rider's LECT
    std::shared_ptr<RiderPoseMesh> riderPose, bikePose;
    std::shared_ptr<LodPoseMesh> bikeLod[4], riderLod[4];
    float attachLod[4][3] = {};
    ModelLayers layers; // the coplanar decals inside each rigid run (coplanar.h)
};

namespace {

void BindMachineSoup(GLuint& vao, GLuint& vbo, const rr::TriangleSoup& soup) {
    gl.GenVertexArrays(1, &vao);
    gl.BindVertexArray(vao);
    gl.GenBuffers(1, &vbo);
    gl.BindBuffer(GL_ARRAY_BUFFER, vbo);
    gl.BufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(soup.vertices.size() * sizeof(rr::TriangleSoup::Vertex)),
                  soup.vertices.data(), GL_DYNAMIC_DRAW);
    const GLsizei stride = static_cast<GLsizei>(sizeof(rr::TriangleSoup::Vertex));
    gl.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
    gl.EnableVertexAttribArray(0);
    gl.VertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(sizeof(float) * 3));
    gl.EnableVertexAttribArray(1);
    gl.VertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(sizeof(float) * 6));
    gl.EnableVertexAttribArray(2);
    gl.VertexAttribPointer(3, 1, GL_UNSIGNED_SHORT, GL_FALSE, stride,
                           reinterpret_cast<void*>(sizeof(float) * 8 + sizeof(uint16_t)));
    gl.EnableVertexAttribArray(3);
    gl.VertexAttribPointer(4, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride,
                           reinterpret_cast<void*>(offsetof(rr::TriangleSoup::Vertex, shade)));
    gl.EnableVertexAttribArray(4);
    gl.VertexAttribPointer(5, 4, GL_UNSIGNED_BYTE, GL_FALSE, stride,
                           reinterpret_cast<void*>(offsetof(rr::TriangleSoup::Vertex, window)));
    gl.EnableVertexAttribArray(5);
}

// a1 -> (bank, block) of BBLEVEL1.TEX, as measured (the file header above).
void PaletteOfA1(int a1, const char*& tag, int& block) {
    tag = "KNBP";
    if (a1 == 29) {
        block = 46;
    } else if (a1 == 30 || a1 == 31) {
        tag = "TSLP";
        block = a1 - 29;
    } else {
        block = 45 - 3 * (a1 / 3) + a1 % 3;
    }
}

} // namespace

void RaceScene::LoadRivalMachines(const rr::DiscImage& disc, const std::vector<std::pair<uint32_t, uint32_t>>& pairs) {
    const auto geoFile = disc.Find("DATA/BBLEVEL1.GEO"); // the file LoadMachine draws the player from
    const auto texFile = disc.Find("DATA/BBLEVEL1.TEX");
    const auto rimFile = disc.Find("DATA/RIMA1.TIM");
    const auto overlay = disc.Find("RASHCDG.BIN");
    if (!geoFile || !overlay) return;
    std::vector<rr::Model> models;
    try {
        models = rr::ParseGeo(disc.ReadFile(*geoFile));
    } catch (const std::exception& e) {
        std::printf("rival machines: BBLEVEL1.GEO: %s - rivals keep the player's machine\n", e.what());
        return;
    }
    const rr::SkeletonTable skeleton = rr::SkeletonTable::FromOverlay(disc.ReadFile(*overlay));
    if (texFile) rivalTex_ = disc.ReadFile(*texFile);
    if (rimFile) rivalRim_ = disc.ReadFile(*rimFile);
    const auto find = [&models](uint32_t id) -> const rr::Model* {
        for (const rr::Model& m : models)
            if (m.id == id && !m.groups.empty()) return &m;
        return nullptr;
    };
    for (const auto& [bikeId, riderId] : pairs) {
        bool have = false;
        for (const auto& m : rivalMachines_) have = have || (m->bikeId == bikeId && m->riderId == riderId);
        if (have) continue;
        const rr::Model* bike = find(bikeId);
        const rr::Model* rider = find(riderId);
        if (bike == nullptr) {
            std::printf("rival machines: model %u is not in BBLEVEL1.GEO - drawn as the player's machine\n", bikeId);
            continue;
        }
        auto set = std::make_shared<RivalMachine>();
        set->bikeId = bikeId;
        set->riderId = rider ? riderId : 0u;
        set->bikeTex = static_cast<uint16_t>(bike->groups.front().slot);
        set->riderTex = rider ? static_cast<uint16_t>(rider->groups.front().slot) : 0;
        rr::TriangleSoup soup;
        // LOD 0 of both through the posing meshes (LoadMachine's appendGroup), LODs 1..3 through lod_pose_draw.h.
        const auto append = [&](const rr::ModelGroup& group, int owner, int lod, int mainSheet, int rimSheet) {
            const size_t base = soup.vertices.size();
            rr::TriangleSoup one;
            if (lod == 0) {
                one = rr::BuildAssembledTriangleSoup(group, rr::AssembleGroup(group, skeleton));
                if (owner == 0) set->bikePose = MakeRiderPoseMesh(group, skeleton, base);
                else set->riderPose = MakeRiderPoseMesh(group, skeleton, base);
            } else {
                std::shared_ptr<LodPoseMesh> mesh =
                    MakeLodPoseMesh(group, skeleton, owner == 0 ? LodOwner::Bike : LodOwner::Rider, base);
                one = PosedLodSoup(*mesh, nullptr, 0);
                (owner == 0 ? set->bikeLod : set->riderLod)[lod] = mesh;
            }
            size_t cursor = 0;
            for (const rr::SubMesh& sub : group.subMeshes) {
                const size_t n = sub.prims.size() * 6;
                if (n != 0) {
                    ModelPart part;
                    part.first = static_cast<GLint>(base + cursor);
                    part.count = static_cast<GLsizei>(n);
                    part.sheet = (sub.prims.front().clut & 0x0080u) ? rimSheet : mainSheet;
                    part.owner = owner;
                    part.lod = lod;
                    SetPartLight(part, group);
                    set->parts.push_back(part);
                }
                cursor += n;
            }
            soup.vertices.insert(soup.vertices.end(), one.vertices.begin(), one.vertices.end());
        };
        for (int lod = 0; lod < 4; ++lod) {
            if (static_cast<size_t>(lod) < bike->groups.size()) append(bike->groups[static_cast<size_t>(lod)], 0, lod, 0, 1);
            if (rider && static_cast<size_t>(lod) < rider->groups.size())
                append(rider->groups[static_cast<size_t>(lod)], 1, lod, 2, 2);
        }
        // The seat per bike LOD (LoadMachine's rule: SeatVertex 0x80066A84, ChildPlace 0x80066B98's fallback).
        const rr::ModelGroup& g0 = bike->groups.front();
        for (int lod = 0; lod < 4; ++lod) {
            const bool own = lod > 0 && static_cast<size_t>(lod) < bike->groups.size();
            const rr::ModelGroup& lg = own ? bike->groups[static_cast<size_t>(lod)] : g0;
            const size_t n = lg.subMeshes.size();
            const int idx = n == 3 ? 2 : n == 5 ? 3 : n == 6 ? 4 : -1;
            const rr::ModelGroup& from = (own && idx >= 0) ? lg : g0;
            if (lg.subMeshes.empty()) continue;
            const size_t at = lg.subMeshes.front().vertBase +
                              static_cast<size_t>((own && idx >= 0) ? idx : (g0.subMeshes.size() < 6 ? 3 : 4));
            if (at >= from.verts.size()) continue;
            const float f = static_cast<float>(rr::LodFactor(from));
            set->attachLod[lod][0] = static_cast<float>(from.verts[at].x) * f;
            set->attachLod[lod][1] = static_cast<float>(from.verts[at].y) * f;
            set->attachLod[lod][2] = static_cast<float>(from.verts[at].z) * f;
        }
        BindMachineSoup(set->vao, set->vbo, soup);
        {
            std::vector<std::pair<GLint, GLsizei>> ranges;
            for (const ModelPart& part : set->parts) ranges.emplace_back(part.first, part.count);
            set->layers.Build(set->vao, soup.vertices, ranges, kModelUnitsPerWorldUnit, true);
        }
        std::printf("rival machine: bike model %u (%zu LODs, LECT %u) + rider model %u (LECT %u): %zu draw runs, %zu "
                    "vertices, seat (%.0f,%.0f,%.0f)\n",
                    bikeId, bike->groups.size(), set->bikeTex, set->riderId, set->riderTex, set->parts.size(),
                    soup.vertices.size(), set->attachLod[0][0], set->attachLod[0][1], set->attachLod[0][2]);
        rivalMachines_.push_back(set);
    }
}

const float* RaceScene::RiderAttachFor(uint32_t bikeModelId, uint32_t riderModelId, int lod) const {
    for (const auto& m : rivalMachines_)
        if (m->bikeId == bikeModelId && m->riderId == riderModelId) return m->attachLod[(lod > 0 && lod < 4) ? lod : 0];
    return RiderAttach(lod);
}

const GpuIndexedTexture* RaceScene::RivalSheet(uint16_t texId, bool rim, int a1) {
    if (rivalTex_.empty() || a1 < 0 || a1 > 63) return nullptr;
    const uint64_t key = (static_cast<uint64_t>(texId) << 16) | (rim ? 0x8000u : 0u) | static_cast<uint64_t>(a1);
    if (const auto it = rivalSheets_.find(key); it != rivalSheets_.end()) return it->second.Valid() ? &it->second : nullptr;
    GpuIndexedTexture& slot = rivalSheets_[key];
    const char* tag = "KNBP";
    int block = 0;
    PaletteOfA1(a1, tag, block);
    try {
        if (rim) {
            if (!rivalRim_.empty()) slot = UploadIndexedTexture(rr::BuildTimSheetWithBank(rivalRim_, rivalTex_, block, tag));
        } else {
            slot = UploadIndexedTexture(rr::BuildSheetWithBank(rivalTex_, texId, block, tag));
        }
    } catch (const std::exception& e) {
        std::printf("rival sheet LECT %u a1 %d: %s (drawn untextured)\n", texId, a1, e.what());
    }
    return slot.Valid() ? &slot : nullptr;
}

bool RaceScene::DrawRivalMachine(const DrawRequest& request, int bikeDebug, int otherDebug) {
    if (request.bikeModelId == 0u || rivalMachines_.empty()) return false;
    RivalMachine* set = nullptr;
    for (const auto& m : rivalMachines_)
        if (m->bikeId == request.bikeModelId && m->riderId == request.riderModelId) set = m.get();
    if (set == nullptr) return false;
    ++rivalMachinesDrawn_;
    const int bikeLod = (request.bikeLod > 0 && request.bikeLod < 4 && set->bikeLod[request.bikeLod]) ? request.bikeLod : 0;
    const int riderLod = (request.riderLod > 0 && request.riderLod < 4 && set->riderLod[request.riderLod]) ? request.riderLod : 0;
    if (riderLod == 0 && set->riderPose) UploadRiderPose(*set->riderPose, set->vbo, request.riderLocal);
    if (bikeLod == 0 && set->bikePose && request.bikeLocal) UploadRiderPose(*set->bikePose, set->vbo, request.bikeLocal);
    if (riderLod > 0) UploadLodPose(*set->riderLod[riderLod], set->vbo, request.riderLocal, 17);
    if (bikeLod > 0) UploadLodPose(*set->bikeLod[bikeLod], set->vbo, request.bikeLocal, 5);
    ApplyCaptured(request, set->bikeId, set->riderId, set->bikePose.get(), bikeLod > 0 ? set->bikeLod[bikeLod].get() : nullptr,
                  set->riderPose.get(), riderLod > 0 ? set->riderLod[riderLod].get() : nullptr, set->vbo, bikeLod,
                  riderLod); // the model draw's own vertices
    DrawShadow(request, otherDebug); // model 100's hull (the file header)
    const Mat4 identity;
    gl.Uniform3f(tintLocation_, 0.78f, 0.74f, 0.66f);
    gl.BindVertexArray(set->vao);
    OtSource(set->vbo); // the ordering-table order (race_scene_ot.cpp)
    OtObject(request.bikeObject);
    for (size_t n = 0; n < set->parts.size(); ++n) {
        const ModelPart& part = set->parts[n];
        if (part.lod != (part.owner == 0 ? bikeLod : riderLod)) continue;
        gl.UniformMatrix4fv(modelLocation_, 1, GL_FALSE, part.owner == 0 ? request.bikeModel.m : request.riderModel.m);
        ApplyModelLight(part, part.owner == 0 ? request.bikeObjFlags : request.riderObjFlags);
        const GpuIndexedTexture* sheet = part.sheet == 2 ? RivalSheet(set->riderTex, false, request.riderA1)
                                                         : RivalSheet(set->bikeTex, part.sheet == 1, request.bikeA1);
        if (sheet != nullptr) {
            BindIndexed(*sheet);
            gl.Uniform1i(debugLocation_, bikeDebug);
        } else {
            gl.Uniform1i(texturedLocation_, 0);
            gl.Uniform1i(debugLocation_, otherDebug);
        }
        gl.Uniform1f(debugIdLocation_, static_cast<float>(n));
        glDrawArrays(GL_TRIANGLES, part.first, part.count);
        DrawModelLayers(set->layers, n); // its decals on top
    }
    gl.Uniform1i(modelLightLocation_, 0);
    gl.Uniform1i(nclipLocation_, 0); // ApplyCaptured set the emitter's back-face test
    gl.UniformMatrix4fv(modelLocation_, 1, GL_FALSE, identity.m);
    gl.Uniform1i(texturedLocation_, 0);
    gl.Uniform1i(debugLocation_, 0);
    return true;
}

} // namespace rr::render
