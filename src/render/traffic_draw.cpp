#include "render/traffic_draw.h"

#include "render/race_scene.h"
#include "rrformats/level_bank.h"
#include "rrformats/level_bundle.h"
#include "rrformats/model_texture.h"
#include "rrformats/rmd3.h"
#include "rrformats/skeleton.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

namespace rr::render {
namespace {

uint32_t Word(const uint8_t* ram, uint32_t a) {
    uint32_t v;
    std::memcpy(&v, ram + (a & 0x1FFFFFu), 4);
    return v;
}
int16_t Half(const uint8_t* ram, uint32_t a) {
    int16_t v;
    std::memcpy(&v, ram + (a & 0x1FFFFFu), 2);
    return v;
}

constexpr uint32_t kPool3Control = 0x800CF650; // live, next free, high water; slots from +16
constexpr uint32_t kPool3Slots = 0x800CF660;
constexpr uint32_t kCarBytes = 512;

} // namespace

bool TrafficDraw::Load(const rr::DiscImage& disc, int raceId, int bank) {
    char name[32];
    std::snprintf(name, sizeof(name), "DATA/CAR%02dA.GEO", raceId % 100);
    file_ = name;
    const auto geo = disc.Find(name);
    const auto overlay = disc.Find("RASHCDG.BIN");
    if (!geo || !overlay) return false;
    std::vector<rr::Model> cars;
    try {
        cars = rr::ParseGeo(disc.ReadFile(*geo));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "traffic models: %s did not parse (%s)\n", name, e.what());
        return false;
    }
    const rr::SkeletonTable skeleton = rr::SkeletonTable::FromOverlay(disc.ReadFile(*overlay));
    rr::TriangleSoup soup;
    for (const rr::Model& model : cars) {
        if (model.groups.empty()) continue;
        std::vector<Range>& lods = models_[model.id];
        for (const rr::ModelGroup& group : model.groups) { // group k = LOD k (the LOD shift folded in)
            const rr::TriangleSoup one = rr::BuildAssembledTriangleSoup(group, rr::AssembleGroup(group, skeleton));
            Range r;
            r.first = static_cast<GLint>(soup.vertices.size());
            r.count = static_cast<GLsizei>(one.vertices.size());
            soup.vertices.insert(soup.vertices.end(), one.vertices.begin(), one.vertices.end());
            r.texId = group.slot; // DOD3+0x1C, the LECT id
            r.group = groups_.size();
            r.model = model.id;
            groups_.push_back(group);
            {   // the model light's inputs (race_scene.h SetPartLight)
                ModelPart lp;
                SetPartLight(lp, group);
                r.lit = lp.lit;
                r.modelClass = lp.modelClass;
            }
            lods.push_back(r);
        }
    }
    if (soup.vertices.empty()) return false;
    soupCpu_ = soup.vertices;
    GteStreamArray(streamVao_, streamVbo_);
    gl.GenVertexArrays(1, &vao_);
    gl.BindVertexArray(vao_);
    gl.GenBuffers(1, &vbo_);
    gl.BindBuffer(GL_ARRAY_BUFFER, vbo_);
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
    { // the coplanar decals of each group (coplanar.h), one soup unit 1/1024 world unit as every model's
        std::vector<std::pair<GLint, GLsizei>> ranges(groups_.size());
        for (const auto& [id, lods] : models_)
            for (const Range& r : lods) ranges[r.group] = {r.first, r.count};
        layers_.Build(vao_, soup.vertices, ranges, 1024.0f, false);
    }
    // The sheets: every LECT chunk (tag, size, ..., +0x10 id) of the level bundle's type-9 section, as
    // RASHCDI 0x8005C920 walks it; the palette bank is BBLEVEL<bank+1>.TEX's KNBP.
    try {
        const auto bin = disc.Find("DATA/GAMEBIN1.DAT");
        if (bin) {
            const std::vector<uint8_t> file = disc.ReadFile(*bin);
            const rr::LevelBundle bundle = rr::ParseLevelBundle(file, rr::LevelBundleIndexForRace(raceId));
            for (const rr::LevelBundleSection& s : bundle.sections) {
                if (s.type != 9) continue;
                const size_t end = s.payload + (s.tag >= 4 ? s.tag - 4u : 0u);
                size_t at = s.payload;
                while (at + 0x20 <= end && end <= file.size()) {
                    uint32_t tag, len, id;
                    std::memcpy(&tag, file.data() + at, 4);
                    std::memcpy(&len, file.data() + at + 4, 4);
                    std::memcpy(&id, file.data() + at + 0x10, 4);
                    if (len == 0 || at + len > end) break;
                    if (tag == 0x5443454Cu && len > 0x18) // "LECT": the TIM from +0x18
                        tims_[id & 0xFFu].assign(file.begin() + static_cast<ptrdiff_t>(at + 0x18),
                                                 file.begin() + static_cast<ptrdiff_t>(at + len));
                    at += len;
                }
            }
        }
        // `bank`: the bundle index 0..4 of rrformats/level_bank.h (RASHCDI 0x8005C45C's name table)
        if (const auto f = disc.Find(rr::LevelBankFile(bank, ".TEX"))) bankTex_ = disc.ReadFile(*f);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "traffic sheets: %s\n", e.what());
    }
    std::printf("traffic models: %s, %zu models, %zu vertices, %zu car sheet(s)\n", name, models_.size(),
                soup.vertices.size(), tims_.size());
    return true;
}

const GpuIndexedTexture* TrafficDraw::Sheet(uint32_t texId, uint32_t a1) {
    const uint64_t key = (static_cast<uint64_t>(texId) << 32) | a1;
    if (const auto it = sheets_.find(key); it != sheets_.end()) return it->second.Valid() ? &it->second : nullptr;
    GpuIndexedTexture& slot = sheets_[key];
    const auto tim = tims_.find(texId);
    if (tim == tims_.end()) return nullptr;
    try {
        // a1 0..28: KNBP block 45 - 3 (a1 / 3) + a1 % 3 (measured, see the header); slots 47 / 46 / 41: the
        // CLUT of sheet 116 / 149 / 151, which RASHCDI 0x8005DDB8 uploads there; otherwise the TIM's own CLUT.
        const bool bankRule = a1 <= 28 && !bankTex_.empty();
        const int block = 45 - 3 * static_cast<int>(a1 / 3) + static_cast<int>(a1 % 3);
        const uint32_t fixedSheet = a1 == 47 ? 116u : a1 == 46 ? 149u : a1 == 41 ? 151u : texId;
        const auto clutTim = tims_.find(fixedSheet);
        const std::vector<uint8_t>& clutFrom = clutTim != tims_.end() ? clutTim->second : tim->second;
        rr::IndexedTexture sheet = rr::BuildTimSheetWithBank(
            tim->second, bankRule ? std::span<const uint8_t>(bankTex_) : std::span<const uint8_t>(), bankRule ? block : 0);
        if (!bankRule && fixedSheet != texId && clutTim != tims_.end()) // that sheet's CLUT under this sheet's image
            sheet.palettes = rr::BuildTimSheetWithBank(clutFrom, std::span<const uint8_t>(), 0).palettes;
        slot = UploadIndexedTexture(sheet);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "traffic sheet 0x%02X: %s\n", texId, e.what());
    }
    return slot.Valid() ? &slot : nullptr;
}

std::vector<TrafficDraw::ProbeCar>& TrafficDraw::ProbeCars() {
    static std::vector<ProbeCar> cars;
    return cars;
}

bool& TrafficDraw::ProbeIds() {
    static bool on = false;
    return on;
}

size_t TrafficDraw::Draw(const RaceScene& scene, const Mat4& viewProj, const uint8_t* ram, float scale,
                         size_t* unknownModel, const std::function<bool(uint32_t)>* keep,
                         const std::function<const CapturedVerts*(uint32_t)>* captured) {
    ProbeCars().clear();
    if (vao_ == 0 || ram == nullptr) return 0;
    const int32_t high = static_cast<int32_t>(Word(ram, kPool3Control + 8));
    if (high < 0) return 0;
    const GLuint program = scene.Program();
    gl.UseProgram(program);
    UploadViewProj(gl.GetUniformLocation(program, "uViewProj"), viewProj); // both eyes in stereo (multiview.h)
    scene.SetTextured(0);
    scene.SetDebug(0);
    scene.SetTint(0.70f, 0.70f, 0.74f); // only where no sheet binds
    const GLint locIndex = gl.GetUniformLocation(program, "uIndex"), locPalette = gl.GetUniformLocation(program, "uPalette");
    const GLint locTexSize = gl.GetUniformLocation(program, "uTexSize");
    const GLint locCount = gl.GetUniformLocation(program, "uPaletteCount"), locSize = gl.GetUniformLocation(program, "uPaletteSize");
    gl.BindVertexArray(vao_);
    scene.OtSource(vbo_); // the ordering-table order (race_scene_ot.cpp)
    size_t drawn = 0;
    for (int32_t s = 0; s <= high && s < 16; ++s) {
        const uint32_t car = kPool3Slots + kCarBytes * static_cast<uint32_t>(s);
        scene.OtObject(car); // its cell's ordering table (race_scene_ot.cpp)
        if ((Word(ram, car + 0xAC) & 0xFFFFu) == 0 || (Word(ram, car + 0x140) & 0xFFFFu) == 0) continue;
        if (keep != nullptr && *keep && !(*keep)(car)) continue; // the cell test and the kind-3 range
        const uint32_t reg = Word(ram, car + 0x60);
        const uint32_t id = (reg >= 0x80000000u && reg < 0x80200000u) ? Word(ram, reg) : 0u;
        const auto it = models_.find(id);
        if (it == models_.end()) {
            if (unknownModel != nullptr) ++*unknownModel;
            continue;
        }
        // The car's frame as the bikes' (tools\rrgame): +0x1B0 lateral, -+0x1B6 normal, +0x1BC tangent.
        float p[3], l[3], n[3], t[3];
        for (uint32_t k = 0; k < 3; ++k) {
            p[k] = static_cast<float>(static_cast<double>(static_cast<int32_t>(Word(ram, car + 0xB8 + 4 * k))) / 65536.0);
            l[k] = static_cast<float>(Half(ram, car + 0x1B0 + 2 * k)) / 4096.0f;
            n[k] = -static_cast<float>(Half(ram, car + 0x1B6 + 2 * k)) / 4096.0f;
            t[k] = static_cast<float>(Half(ram, car + 0x1BC + 2 * k)) / 4096.0f;
        }
        const float sense = n[1] <= 0.0f ? 1.0f : -1.0f;
        float up[3];
        RoadFrame f;
        for (int k = 0; k < 3; ++k) {
            up[k] = n[k] * sense;
            f.pos[k] = p[k];
            f.tangent[k] = t[k];
            f.lateral[k] = l[k];
            f.normal[k] = n[k];
        }
        // the level of detail +0x08 (model_runtime.h: ModelLod / ModelVisible's LodSelect)
        const int8_t lodByte = static_cast<int8_t>(ram[(car + 8u) & 0x1FFFFFu]);
        // forceLod0: the PC graphics settings' maximum detail - the full model at any distance
        const size_t lod = lodByte < 0 || forceLod0 ? 0u : std::min(static_cast<size_t>(lodByte), it->second.size() - 1u);
        const Range& range = it->second[lod];
        // the sheet with the object's palette a1 = +0x24 bits 12..17
        const uint32_t a1 = (Word(ram, car + 0x24) >> 12) & 0x3Fu;
        // DEVELOPMENT: RRJB_CARFLICK_FLAT=1 - the cars untextured (the brightness meter's texture-free control)
        static const bool flat = std::getenv("RRJB_CARFLICK_FLAT") != nullptr;
        if (const GpuIndexedTexture* sh = flat ? nullptr : Sheet(range.texId, a1)) {
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
            if (a1 > 28) ++ownClut_;
        } else {
            scene.SetTextured(0);
        }
        const Mat4 model = MachineMatrixAt(f, up, p, scale);
        scene.SetModelMatrix(model);
        scene.ApplyObjectLight(range.lit, range.modelClass, RaceScene::LookLightOff() ? -1 : static_cast<int64_t>(Word(ram, car + 0x24))); // the model light
        { // DEVELOPMENT (carflick_probe.cpp): what was drawn, and the id mask
            ProbeCar pc;
            pc.slot = s;
            pc.model = id;
            for (int k = 0; k < 3; ++k) pc.pos[k] = p[k];
            pc.lodByte = lodByte;
            pc.flags = Word(ram, car + 0x24);
            ProbeCars().push_back(pc);
            if (ProbeIds()) {
                scene.SetDebug(2);
                gl.Uniform1f(gl.GetUniformLocation(program, "uDebugId"), static_cast<float>(s + 1));
            }
        }
        // the car at the PORTED model draw's own SXY when it was captured at this LOD
        const CapturedVerts* c = (captured != nullptr && *captured) ? (*captured)(car) : nullptr;
        if (c != nullptr && range.group < groups_.size() &&
            scene.GteObjectSoup(c, range.model, static_cast<int>(lod), model, groups_[range.group],
                                soupCpu_.data() + range.first, static_cast<size_t>(range.count), gteSoup_)) {
            GteStreamUpload(streamVao_, streamVbo_, gteSoup_);
            scene.OtSource(streamVbo_);
            glDrawArrays(GL_TRIANGLES, 0, range.count);
            gl.BindVertexArray(vao_);
            scene.OtSource(vbo_);
        } else {
            glDrawArrays(GL_TRIANGLES, range.first, range.count);
            scene.DrawModelLayers(layers_, range.group); // its decals on top
        }
        if (lod < 4) ++lodDraws_[lod];
        ++drawn;
    }
    scene.ClearObjectLight();
    scene.SetModelMatrix(Mat4{});
    scene.SetTextured(0);
    if (ProbeIds()) scene.SetDebug(0);
    return drawn;
}

} // namespace rr::render
