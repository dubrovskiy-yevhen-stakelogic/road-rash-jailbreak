// The two-seat machine in the race scene (race_scene.h LoadSidecar / DrawSidecar; docs\formats\rules.md 16).
//
// What the original loads, from our own disassembly of RASHCDI.BIN (SHA-1 9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06,
// loaded at 0x8005B5E8): LoadBikeBank 0x8005C45C, after the level bank's .GEO / .TEX (0x8005CA10), walks the
// players (game_state+0x30) and for a bike index game_state+0x48 + 4p in 6..8 or 15..17 (0x8005C570..0x8005C584)
// whose model 100 + index is not registered yet (0x8005C010) builds "DATA\<name>.MRO" with <name> the 9-byte
// entry `index` of the table 0x8006B51C (0x80064034(0, index): CRUISES1..3 / SPORTS1..3) and hands it to
// 0x8005C30C, which uploads the leading LECT chunk and registers the RMD3 model (0x8005C0C4, a3 = 1). ModelBind
// then gives the player's bike (+0xB4 = the index) that model: the whole rig, bike and car, 6 parts at LOD 0.
//
// THE PARTS: the rig is bound to the player's bike object (model_runtime.cpp loads the .MRO into the model arena,
// jail_session.cpp JailRigParts gives the bike RegistryBind's six part slots), so BikeInstance 0x80084E10 (fork
// slot 1, pitch slot 2) and BikeWheels 0x80066EC4 (wheels 3 / 4, and with six parts slot 5 from the passenger
// bike's +0x344) animate it; LOD 0 is re-posed from those slots (DrawRequest::sidecarLocal), LODs 1..3 rest.
//
// THE SHEET AND PALETTE (rules.md 16.5): 0x8005C30C hands the MRO's LECT to 0x8005DDB8, whose kind 1..3 arm
// returns at once when the page table already holds that id - and the level bundle, loaded first (0x8005CA10),
// carries it (BBLEVJBD.TEX id 112 = CRUISES3.MRO's LECT, byte for byte) - so the image is the page of the rig's
// DOD3+0x1C. An 8bpp object's palette is its own a1 (+0x24 bits 12..17): SpawnBike
// RASHCDI 0x80065A94 gives a player's bike a1 = the appearance slot 0x8005DD9C(p) (race types even or 0x21),
// which the TSLP handler 0x8005DBB8 filled with TSLP blocks 6u (cruiser) / 6u + 3 (sport), u the player's
// record +0x0A: bike slot, rider slot + 1, passenger (SpawnPassenger 0x800670FC) slot + 2. The wheels (prim.clut
// bit 7) take the rim sheet RIMA1.TIM under the same palette.
#include "render/race_scene.h"

#include "render/lod_pose_draw.h"
#include "render/rider_pose_draw.h"
#include "render/shaders.h"
#include "rrformats/level_bank.h"
#include "rrformats/model_texture.h"
#include "rrformats/rmd3.h"
#include "rrformats/skeleton.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>

namespace rr::render {
namespace {

// race_scene.cpp's BindSoup (the machine's vertex layout), for the rig's own buffer.
void BindRigSoup(GLuint& vao, GLuint& vbo, const rr::TriangleSoup& soup) {
    gl.GenVertexArrays(1, &vao);
    gl.BindVertexArray(vao);
    gl.GenBuffers(1, &vbo);
    gl.BindBuffer(GL_ARRAY_BUFFER, vbo);
    gl.BufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(soup.vertices.size() * sizeof(rr::TriangleSoup::Vertex)),
                  soup.vertices.data(), GL_STATIC_DRAW);
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

// RASHCDI 0x8006B51C: the bike names, 9 bytes apart (0x80064034 with a0 = 0).
std::string BikeName(uint32_t index) {
    if (index >= 18u) return {};
    const char* family = index < 9u ? "CRUISE" : "SPORT";
    const char series = "ABS"[(index % 9u) / 3u];
    return std::string(family) + series + static_cast<char>('1' + index % 3u);
}

} // namespace

bool RaceScene::LoadSidecar(const rr::DiscImage& disc, uint32_t bikeIndex, int bundle, int paletteBlock) {
    if (!((bikeIndex - 6u) < 3u || (bikeIndex - 15u) < 3u)) return false; // 0x8005C570..0x8005C584
    const std::string file = "DATA/" + BikeName(bikeIndex) + ".MRO";
    const auto mro = disc.Find(file);
    const auto overlay = disc.Find("RASHCDG.BIN");
    const auto texFile = disc.Find(rr::LevelBankFile(bundle, ".TEX")); // the race's level bundle (level_bank.h)
    const auto rimFile = disc.Find("DATA/RIMA1.TIM");
    const auto riderGeo = disc.Find("DATA/BBLEVEL1.GEO"); // the rider mesh LoadMachine draws (model 150)
    if (!mro || !overlay) {
        std::printf("sidecar: %s is not on the disc: the rig is NOT drawn\n", file.c_str());
        return false;
    }
    const std::vector<uint8_t> bytes = disc.ReadFile(*mro);
    std::vector<rr::Model> models;
    try {
        models = rr::ParseMro(bytes);
    } catch (const std::exception& e) {
        std::printf("sidecar: %s: %s: the rig is NOT drawn\n", file.c_str(), e.what());
        return false;
    }
    const rr::Model* rig = nullptr;
    for (const rr::Model& m : models)
        if (m.id == 100u + bikeIndex) rig = &m;
    if (rig == nullptr || rig->groups.empty()) {
        std::printf("sidecar: %s holds no model %u: the rig is NOT drawn\n", file.c_str(), 100u + bikeIndex);
        return false;
    }
    const rr::SkeletonTable skeleton = rr::SkeletonTable::FromOverlay(disc.ReadFile(*overlay));

    // The sheet: the bundle's TEX first, then the MRO, so the chunk walk finds the page the bundle already
    // holds for the rig's id (0x8005DDB8's early return) and else the MRO's own; the TSLP bank is the bundle's.
    std::vector<uint8_t> chain;
    if (texFile) chain = disc.ReadFile(*texFile);
    const size_t bundleBytes = chain.size();
    chain.insert(chain.end(), bytes.begin(), bytes.end());
    sidecarSheets_.clear();
    const uint16_t rigTex = static_cast<uint16_t>(rig->groups.front().slot);
    bool fromBundle = false;
    for (size_t at = 0; at + 0x14 <= bundleBytes;) {
        uint32_t tag = 0, len = 0;
        std::memcpy(&tag, chain.data() + at, 4);
        std::memcpy(&len, chain.data() + at + 4, 4);
        if (len == 0) break;
        if (tag == 0x5443454Cu && chain[at + 0x10] == rigTex) fromBundle = true;
        at += len;
    }
    uint32_t riderTex = 0;
    if (riderGeo) {
        try {
            for (const rr::Model& m : rr::ParseGeo(disc.ReadFile(*riderGeo)))
                if (m.id == 150u && !m.groups.empty()) riderTex = m.groups.front().slot;
        } catch (const std::exception&) {
            riderTex = 0;
        }
    }
    try {
        sidecarSheets_.push_back(UploadIndexedTexture(rr::BuildSheetWithBank(chain, rigTex, paletteBlock, "TSLP")));
        if (rimFile)
            sidecarSheets_.push_back(UploadIndexedTexture(
                rr::BuildTimSheetWithBank(disc.ReadFile(*rimFile), chain, paletteBlock, "TSLP")));
        else
            sidecarSheets_.push_back(GpuIndexedTexture{});
        if (riderTex != 0u)
            sidecarSheets_.push_back(UploadIndexedTexture(
                rr::BuildSheetWithBank(chain, static_cast<uint16_t>(riderTex), paletteBlock + 2, "TSLP")));
    } catch (const std::exception& e) {
        std::printf("sidecar: the rig's sheet: %s (drawn untextured)\n", e.what());
    }

    rr::TriangleSoup soup;
    sidecarParts_.clear();
    const rr::ModelGroup& g0 = rig->groups.front();
    for (size_t lod = 0; lod < rig->groups.size() && lod < 4u; ++lod) {
        const rr::ModelGroup& group = rig->groups[lod];
        if (group.subMeshes.empty()) continue;
        const rr::TriangleSoup one = rr::BuildAssembledTriangleSoup(group, rr::AssembleGroup(group, skeleton));
        const size_t base = soup.vertices.size();
        // The lower LODs' mesh, only to take the PORTED model draw's vertices (UploadCapturedGroup)
        if (lod > 0) sidecarLod_[lod] = MakeLodPoseMesh(group, skeleton, LodOwner::Bike, base);
        size_t cursor = 0;
        for (const rr::SubMesh& sub : group.subMeshes) {
            const size_t n = sub.prims.size() * 6;
            if (n != 0) {
                ModelPart part;
                part.first = static_cast<GLint>(base + cursor);
                part.count = static_cast<GLsizei>(n);
                part.sheet = (sub.prims.front().clut & 0x0080u) ? 1 : 0; // 1: the rim sheet (LoadMachine's sheet 1)
                part.owner = 0;
                part.lod = static_cast<int>(lod);
                SetPartLight(part, group); // the model light's inputs
                sidecarParts_.push_back(part);
            }
            cursor += n;
        }
        soup.vertices.insert(soup.vertices.end(), one.vertices.begin(), one.vertices.end());
        // The seat (race_scene.cpp LoadMachine's rule, SeatVertex RASHCDG 0x80066A84 / ChildPlace 0x80066B98):
        // sub-mesh 0's vertex 2 / 3 / 4 for 3 / 5 / 6 parts of this LOD, else LOD 0's + 3 (under 6 parts) / + 4.
        const size_t parts = group.subMeshes.size();
        const int idx = parts == 3 ? 2 : parts == 5 ? 3 : parts == 6 ? 4 : -1;
        const rr::ModelGroup& from = (idx >= 0 && lod > 0) ? group : g0;
        const size_t at = group.subMeshes.front().vertBase +
                          static_cast<size_t>(idx >= 0 && lod > 0 ? idx : (g0.subMeshes.size() < 6 ? 3 : 4));
        float* out = sidecarAttachLod_[lod];
        if (at < from.verts.size()) {
            const float f = static_cast<float>(rr::LodFactor(from));
            out[0] = static_cast<float>(from.verts[at].x) * f;
            out[1] = static_cast<float>(from.verts[at].y) * f;
            out[2] = static_cast<float>(from.verts[at].z) * f;
        }
    }
    for (size_t lod = rig->groups.size(); lod < 4u; ++lod)
        for (int k = 0; k < 3; ++k) sidecarAttachLod_[lod][k] = sidecarAttachLod_[0][k];
    if (sidecarParts_.empty()) return false;
    BindRigSoup(sidecarVao_, sidecarVbo_, soup);
    sidecarPose_ = MakeRiderPoseMesh(g0, skeleton, 0); // LOD 0 leads the rig's buffer
    sidecarPoseParts_ = g0.subMeshes.size();
    sidecarModel_ = rig->id;
    const std::string tex = rr::LevelBankFile(bundle, ".TEX");
    std::printf("sidecar: %s model %u (%zu LOD(s), %zu parts at LOD 0, %zu draw runs, %zu vertices), sheet LECT %u "
                "from %s%s, palette TSLP block %d of %s (rim the same, passenger rider block %d), seat (%.0f,%.0f,%.0f)\n",
                file.c_str(), rig->id, rig->groups.size(), g0.subMeshes.size(), sidecarParts_.size(), soup.vertices.size(),
                g0.slot, fromBundle ? tex.c_str() : file.c_str(), sidecarSheets_.empty() ? " NOT built" : "",
                paletteBlock, tex.c_str(), paletteBlock + 2, sidecarAttachLod_[0][0], sidecarAttachLod_[0][1],
                sidecarAttachLod_[0][2]);
    return true;
}

size_t RaceScene::SidecarParts() const { return sidecarPose_ ? sidecarPoseParts_ : 0u; }

void RaceScene::DrawSidecar(const DrawRequest& request, int bikeDebug, int otherDebug, int bikeLod) {
    int lod = (bikeLod > 0 && bikeLod < 4) ? bikeLod : 0;
    bool any = false;
    for (const ModelPart& p : sidecarParts_) any = any || p.lod == lod;
    if (!any) lod = 0;
    if (lod == 0 && sidecarPose_) UploadRiderPose(*sidecarPose_, sidecarVbo_, request.sidecarLocal); // the rig's slots
    // The rig IS the bike object (model 100 + index), so the PORTED ModelDraw's camera-space vertices of it
    // (request.bikeCaptured, shadow_product.h) give every part where the original's emitter puts it, as for the
    // other machines (race_scene_shadow.cpp ApplyCaptured). A lower LOD taken from them and later drawn without
    // one goes back to its program's rest pose. RRJB_POSE_FROM=ours: the slots' posing (the control).
    if (request.bikeCaptured != nullptr) {
        LodPoseMesh* low = lod > 0 ? sidecarLod_[lod].get() : nullptr;
        const bool ok = UploadCapturedGroup(request.bikeCaptured, sidecarModel_, request.bikeModel, sidecarPose_.get(), low,
                                            sidecarVbo_, lod);
        ++(ok ? sidecarCaptured_ : sidecarCaptureMissed_);
        if (ok && lod > 0) sidecarLodDirty_[lod] = true;
        if (!ok && lod > 0 && low != nullptr && sidecarLodDirty_[lod]) {
            UploadLodPose(*low, sidecarVbo_, nullptr, 0);
            sidecarLodDirty_[lod] = false;
        }
    }
    gl.BindVertexArray(sidecarVao_);
    OtSource(sidecarVbo_); // the ordering-table order (race_scene_ot.cpp)
    OtObject(request.bikeObject);
    gl.UniformMatrix4fv(modelLocation_, 1, GL_FALSE, request.bikeModel.m);
    for (size_t n = 0; n < sidecarParts_.size(); ++n) {
        const ModelPart& part = sidecarParts_[n];
        if (part.lod != lod) continue;
        const GpuIndexedTexture* sheet = nullptr;
        if (part.sheet == 0 && !sidecarSheets_.empty() && sidecarSheets_[0].Valid()) sheet = &sidecarSheets_[0];
        if (part.sheet == 1 && sidecarSheets_.size() > 1 && sidecarSheets_[1].Valid()) sheet = &sidecarSheets_[1];
        if (sheet != nullptr) {
            BindIndexed(*sheet);
            gl.Uniform1i(debugLocation_, bikeDebug);
        } else {
            gl.Uniform1i(texturedLocation_, 0);
            gl.Uniform1i(debugLocation_, otherDebug);
        }
        gl.Uniform1f(debugIdLocation_, static_cast<float>(n));
        ApplyModelLight(part, LookLightOff() ? -1 : request.bikeObjFlags); // the rig is the bike object
        glDrawArrays(GL_TRIANGLES, part.first, part.count);
    }
    gl.Uniform1i(modelLightLocation_, 0);
    if (!request.passenger) return;
    // The seat-1 rider: the machine's rider mesh re-posed with the passenger's slots, drawn at its matrix.
    const int riderLod = (request.passengerLod > 0 && request.passengerLod < 4 && riderLod_[request.passengerLod])
                             ? request.passengerLod : 0;
    if (riderLod == 0 && riderPose_) UploadRiderPose(*riderPose_, bikeVbo_, request.passengerLocal);
    if (riderLod > 0) UploadLodPose(*riderLod_[riderLod], bikeVbo_, request.passengerLocal, 17);
    bool captured = false;
    if (request.passengerCaptured != nullptr) { // the passenger from the PORTED model draw, as the player's rider
        captured = UploadCapturedGroup(request.passengerCaptured, 150u, request.passengerModel, riderPose_.get(),
                                       riderLod > 0 ? riderLod_[riderLod].get() : nullptr, bikeVbo_, riderLod);
        ++(captured ? passengerCaptured_ : sidecarCaptureMissed_);
    }
    if (!captured && request.passengerLocal == nullptr) return; // no part slots and no capture: nothing to pose it from
    gl.BindVertexArray(bikeVao_);
    OtSource(bikeVbo_);
    OtObject(request.bikeObject);
    gl.UniformMatrix4fv(modelLocation_, 1, GL_FALSE, request.passengerModel.m);
    for (size_t n = 0; n < bikeParts_.size(); ++n) {
        const ModelPart& part = bikeParts_[n];
        if (part.owner != 1 || part.lod != riderLod) continue;
        // the rider's own sheet (LoadMachine's sheet 2) under the passenger's palette, TSLP block + 2
        const GpuIndexedTexture* own = (part.sheet == 2 && sidecarSheets_.size() > 2 && sidecarSheets_[2].Valid())
                                           ? &sidecarSheets_[2] : nullptr;
        const bool textured = own != nullptr || (static_cast<size_t>(part.sheet) < riderSheetTextures_.size() &&
                                                 riderSheetTextures_[static_cast<size_t>(part.sheet)].Valid());
        if (textured) {
            BindIndexed(own != nullptr ? *own : riderSheetTextures_[static_cast<size_t>(part.sheet)]);
            gl.Uniform1i(debugLocation_, bikeDebug);
        } else {
            gl.Uniform1i(texturedLocation_, 0);
            gl.Uniform1i(debugLocation_, otherDebug);
        }
        gl.Uniform1f(debugIdLocation_, static_cast<float>(n));
        ApplyModelLight(part, LookLightOff() ? -1 : request.passengerObjFlags); // the passenger object's flags
        glDrawArrays(GL_TRIANGLES, part.first, part.count);
    }
    gl.Uniform1i(modelLightLocation_, 0);
}

} // namespace rr::render
