#include "render/ped_draw.h"

#include "render/lod_pose_draw.h"
#include "render/race_scene.h"
#include "rrformats/level_bundle.h"
#include "rrformats/model_texture.h"
#include "rrformats/rmd3.h"
#include "rrformats/skeleton.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <exception>

namespace rr::render {

bool PedDraw::Load(const rr::DiscImage& disc, int raceId) {
    const auto geo = disc.Find("DATA/PED01A.GEO");
    const auto overlay = disc.Find("RASHCDG.BIN");
    if (!geo || !overlay) return false;
    std::vector<rr::Model> models;
    try {
        models = rr::ParseGeo(disc.ReadFile(*geo));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "pedestrian models: PED01A.GEO did not parse (%s)\n", e.what());
        return false;
    }
    const rr::SkeletonTable skeleton = rr::SkeletonTable::FromOverlay(disc.ReadFile(*overlay));
    for (const rr::Model& m : models) {
        if (m.groups.empty()) continue;
        for (const rr::ModelGroup& g : m.groups) { // every LOD (the PORTED ModelVisible picks it)
            meshes_[m.id].push_back(MakeLodPoseMesh(g, skeleton, LodOwner::Rider, 0));
            groups_[m.id].push_back(g);
        }
        modelSheet_[m.id] = m.groups[0].slot;
        ModelPart lp; // the model light's inputs (race_scene.h SetPartLight)
        SetPartLight(lp, m.groups[0]);
        modelLight_[m.id] = {lp.lit, lp.modelClass};
    }
    if (meshes_.empty()) return false;
    try { // every LECT chunk (tag, size, ..., +0x10 id, the TIM from +0x18) of the bundle's sections 8 and 9
        if (const auto bin = disc.Find("DATA/GAMEBIN1.DAT")) {
            const std::vector<uint8_t> file = disc.ReadFile(*bin);
            const rr::LevelBundle bundle = rr::ParseLevelBundle(file, rr::LevelBundleIndexForRace(raceId));
            for (const rr::LevelBundleSection& s : bundle.sections) {
                if (s.type != 8 && s.type != 9) continue;
                const size_t end = s.payload + (s.tag >= 4 ? s.tag - 4u : 0u);
                size_t at = s.payload;
                while (at + 0x20 <= end && end <= file.size()) {
                    uint32_t tag, len, id;
                    std::memcpy(&tag, file.data() + at, 4);
                    std::memcpy(&len, file.data() + at + 4, 4);
                    std::memcpy(&id, file.data() + at + 0x10, 4);
                    if (len == 0 || at + len > end) break;
                    if (tag == 0x5443454Cu && len > 0x18 && tims_.count(id & 0xFFu) == 0)
                        tims_[id & 0xFFu].assign(file.begin() + static_cast<ptrdiff_t>(at + 0x18),
                                                 file.begin() + static_cast<ptrdiff_t>(at + len));
                    at += len;
                }
            }
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "pedestrian sheets: %s\n", e.what());
    }
    gl.GenVertexArrays(1, &vao_);
    gl.BindVertexArray(vao_);
    gl.GenBuffers(1, &vbo_);
    gl.BindBuffer(GL_ARRAY_BUFFER, vbo_);
    gl.BufferData(GL_ARRAY_BUFFER, 16, nullptr, GL_DYNAMIC_DRAW);
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
    gl.BindVertexArray(0);
    std::printf("pedestrian models: PED01A.GEO, %zu model(s), %zu sheet(s) in the bundle\n", meshes_.size(), tims_.size());
    return true;
}

const GpuIndexedTexture* PedDraw::Sheet(uint32_t id) {
    if (const auto it = sheets_.find(id); it != sheets_.end()) return it->second.Valid() ? &it->second : nullptr;
    GpuIndexedTexture& slot = sheets_[id];
    const auto tim = tims_.find(id);
    if (tim == tims_.end()) return nullptr;
    try {
        slot = UploadIndexedTexture(rr::BuildTimSheetWithBank(tim->second, std::span<const uint8_t>(), 0)); // its own CLUT
    } catch (const std::exception& e) {
        std::fprintf(stderr, "pedestrian sheet 0x%02X: %s\n", id, e.what());
    }
    return slot.Valid() ? &slot : nullptr;
}

size_t PedDraw::Draw(const RaceScene& scene, const Mat4& viewProj, const std::vector<PedInstance>& peds) {
    if (vao_ == 0 || peds.empty()) return 0;
    const GLuint program = scene.Program();
    gl.UseProgram(program);
    UploadViewProj(gl.GetUniformLocation(program, "uViewProj"), viewProj); // both eyes in stereo (multiview.h)
    scene.SetDebug(0);
    scene.SetTint(0.70f, 0.66f, 0.62f); // only where no sheet binds
    const GLint locIndex = gl.GetUniformLocation(program, "uIndex"), locPalette = gl.GetUniformLocation(program, "uPalette");
    const GLint locTexSize = gl.GetUniformLocation(program, "uTexSize");
    const GLint locCount = gl.GetUniformLocation(program, "uPaletteCount"), locSize = gl.GetUniformLocation(program, "uPaletteSize");
    gl.BindVertexArray(vao_);
    gl.BindBuffer(GL_ARRAY_BUFFER, vbo_);
    size_t drawn = 0;
    for (const PedInstance& p : peds) {
        const auto mesh = meshes_.find(p.model);
        if (mesh == meshes_.end() || mesh->second.empty()) {
            ++unknown_;
            continue;
        }
        const size_t lod = static_cast<size_t>(std::clamp(p.lod, 0, static_cast<int>(mesh->second.size()) - 1));
        rr::TriangleSoup soup = PosedLodSoup(*mesh->second[lod], p.slots, 17);
        if (soup.vertices.empty()) continue;
        const uint32_t sheetId = p.sheet != 0 ? p.sheet : modelSheet_[p.model];
        if (const GpuIndexedTexture* sh = Sheet(sheetId)) {
            scene.SetTextured(1);
            gl.ActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, sh->indexTexture);
            gl.Uniform1i(locIndex, 0);
            gl.ActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, sh->paletteTexture);
            gl.Uniform1i(locPalette, 1);
            gl.ActiveTexture(GL_TEXTURE0);
            gl.Uniform2f(locTexSize, sh->width, sh->height);
            gl.Uniform1f(locCount, sh->paletteCount);
            gl.Uniform1f(locSize, sh->paletteSize);
            scene.BindSmooth(*sh); // the PC graphics settings' smooth textures (race_scene_pc.cpp)
            ++textured_;
        } else {
            scene.SetTextured(0);
        }
        // model axis c = row c of +0x1B0 (the part-0 matrix +0x68 = the rows transposed), model units / 1024
        Mat4 m;
        float centre[3];
        for (int k = 0; k < 3; ++k) centre[k] = static_cast<float>(static_cast<double>(p.pos[k]) / 65536.0);
        for (int c = 0; c < 3; ++c) {
            for (int k = 0; k < 3; ++k) {
                const float axis = static_cast<float>(p.rows[3 * c + k]) / 4096.0f;
                m.m[4 * c + k] = axis / kModelUnitsPerWorldUnit;
                centre[k] += axis * static_cast<float>(p.root[c]) / kModelUnitsPerWorldUnit;
            }
            m.m[4 * c + 3] = 0.0f;
        }
        for (int k = 0; k < 3; ++k) m.m[12 + k] = centre[k];
        m.m[15] = 1.0f;
        // at the PORTED model draw's own SXY when it drew this LOD
        const auto& groups = groups_[p.model];
        const bool gte = p.captured != nullptr && lod < groups.size() &&
                         scene.GteObjectSoup(p.captured, p.model, static_cast<int>(lod), m, groups[lod], soup.vertices.data(),
                                             soup.vertices.size(), gteSoup_);
        const std::vector<rr::TriangleSoup::Vertex>& verts = gte ? gteSoup_ : soup.vertices;
        gl.BufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(rr::TriangleSoup::Vertex)), verts.data(),
                      GL_DYNAMIC_DRAW);
        scene.SetModelMatrix(m);
        scene.ApplyObjectLight(modelLight_[p.model].first, modelLight_[p.model].second, RaceScene::LookLightOff() ? -1 : p.flags); // the model light
        scene.OtSource(vbo_); // the ordering-table order (race_scene_ot.cpp): this pedestrian's buffer
        scene.OtObject(p.entity);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(soup.vertices.size()));
        ++drawn;
    }
    scene.ClearObjectLight();
    scene.SetModelMatrix(Mat4{});
    scene.SetTextured(0);
    gl.BindVertexArray(0);
    return drawn;
}

} // namespace rr::render
