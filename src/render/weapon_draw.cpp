#include "render/weapon_draw.h"

#include "render/race_scene.h"
#include "rrformats/model_texture.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <exception>
#include <span>

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

constexpr uint32_t kObjSlots = 0x800CF018; // the eight weapon model objects, 172 bytes each (weapon.h)
constexpr uint32_t kObjBytes = 172;
constexpr uint32_t kWeaponModel = 800, kRiderModel = 150;

WeaponHandOverride g_handOverride; // VR only (weapon_draw.h)

// The weapon soup's model matrix in a tracked hand (WeaponHandOverride): its long axis onto `along`.
Mat4 HandModel(const rr::TriangleSoup& soup, const WeaponHandOverride& o) {
    if (o.useMatrix) return o.matrix; // the calibrated grip (tools\rrgame\vr_weapon_calib.h)
    float a[3] = {0, -1, 0}, far2 = 0.0f;
    for (const auto& v : soup.vertices) {
        const float d2 = v.x * v.x + v.y * v.y + v.z * v.z;
        if (d2 > far2) {
            far2 = d2;
            a[0] = v.x, a[1] = v.y, a[2] = v.z;
        }
    }
    const auto norm = [](float v[3]) {
        const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (l > 1e-6f)
            for (int k = 0; k < 3; ++k) v[k] /= l;
    };
    const auto cross = [](const float x[3], const float y[3], float out[3]) {
        out[0] = x[1] * y[2] - x[2] * y[1];
        out[1] = x[2] * y[0] - x[0] * y[2];
        out[2] = x[0] * y[1] - x[1] * y[0];
    };
    norm(a);
    float p[3] = {1, 0, 0};
    if (std::fabs(a[0]) > 0.9f) p[0] = 0, p[2] = 1;
    const float ap = a[0] * p[0] + a[1] * p[1] + a[2] * p[2];
    for (int k = 0; k < 3; ++k) p[k] -= ap * a[k];
    norm(p);
    float q[3], wa[3], wp[3], wq[3];
    cross(a, p, q);
    for (int k = 0; k < 3; ++k) {
        wa[k] = o.along[k];
        wp[k] = o.side[k];
    }
    norm(wa);
    const float wap = wa[0] * wp[0] + wa[1] * wp[1] + wa[2] * wp[2];
    for (int k = 0; k < 3; ++k) wp[k] -= wap * wa[k];
    norm(wp);
    cross(wa, wp, wq);
    Mat4 m; // column j: the image of the weapon's axis j = wa a[j] + wp p[j] + wq q[j]
    for (int j = 0; j < 3; ++j) {
        for (int k = 0; k < 3; ++k) m.m[j * 4 + k] = (wa[k] * a[j] + wp[k] * p[j] + wq[k] * q[j]) * o.scale;
        m.m[j * 4 + 3] = 0.0f;
    }
    for (int k = 0; k < 3; ++k) m.m[12 + k] = o.origin[k];
    m.m[15] = 1.0f;
    return m;
}

} // namespace

std::vector<ExtraWeaponDraw> g_extraWeapons; // VR only: the holstered weapons

void SetWeaponHandOverride(const WeaponHandOverride& o) { g_handOverride = o; }
void SetExtraWeapons(const std::vector<ExtraWeaponDraw>& list) { g_extraWeapons = list; }
Mat4 WeaponHandMatrix(const rr::TriangleSoup& soup, const WeaponHandOverride& o) { return HandModel(soup, o); }

bool ChainShapeOf(const rr::ModelGroup& g, const rr::SkeletonTable& skeleton, const rr::Assembly& as, ChainShape& out) {
    out = ChainShape{};
    const size_t parts = g.subMeshes.size();
    if (!as.assembled || parts < 2 || parts > 4 || as.programIndex >= skeleton.Programs().size()) return false;
    const rr::AttachProgram& p = skeleton.Programs()[as.programIndex];
    if (p.links.size() != parts - 1) return false;
    const int factor = rr::LodFactor(g);
    for (size_t k = 0; k + 1 < parts; ++k) { // part k's length: its link vertex, where part k + 1 hangs
        const rr::AttachLink& l = p.links[k];
        const size_t at = g.subMeshes[k].vertBase + l.vertexOffset;
        if (static_cast<size_t>(l.parent) != k || static_cast<size_t>(l.matrixPart) != k + 1 || at >= g.verts.size()) return false;
        const float v[3] = {static_cast<float>(g.verts[at].x * factor), static_cast<float>(g.verts[at].y * factor),
                            static_cast<float>(g.verts[at].z * factor)};
        const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (!(len > 0.0f)) return false;
        out.length[k] = len;
        for (int c = 0; c < 3; ++c) out.axis[k][c] = v[c] / len;
    }
    {   // the last part: its vertex extent's long axis, to its far end
        const rr::SubMesh& sm = g.subMeshes[parts - 1];
        float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
        for (uint32_t i = 0; i < sm.vertCount && sm.vertBase + i < g.verts.size(); ++i) {
            const rr::SVector& v = g.verts[sm.vertBase + i];
            const float q[3] = {static_cast<float>(v.x * factor), static_cast<float>(v.y * factor), static_cast<float>(v.z * factor)};
            for (int c = 0; c < 3; ++c) lo[c] = std::min(lo[c], q[c]), hi[c] = std::max(hi[c], q[c]);
        }
        int ax = 0;
        for (int c = 1; c < 3; ++c)
            if (hi[c] - lo[c] > hi[ax] - lo[ax]) ax = c;
        const bool neg = std::fabs(lo[ax]) > std::fabs(hi[ax]);
        out.axis[parts - 1][ax] = neg ? -1.0f : 1.0f;
        out.length[parts - 1] = neg ? std::fabs(lo[ax]) : std::fabs(hi[ax]);
        if (!(out.length[parts - 1] > 0.0f)) return false;
    }
    out.parts = static_cast<int>(parts);
    return true;
}

void ChainSlots(const Mat4& model, const ChainShape& shape, const float chain[4][3], rr::PartMatrix out[4]) {
    // 3x3 row-major helpers
    struct M3 {
        float r[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    };
    const auto mul = [](const M3& a, const M3& b) {
        M3 o;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                float s = 0.0f;
                for (int k = 0; k < 3; ++k) s += a.r[i * 3 + k] * b.r[k * 3 + j];
                o.r[i * 3 + j] = s;
            }
        return o;
    };
    const auto tr = [](const M3& a) {
        M3 o;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) o.r[i * 3 + j] = a.r[j * 3 + i];
        return o;
    };
    const auto apply = [](const M3& a, const float v[3], float o[3]) {
        for (int i = 0; i < 3; ++i) o[i] = a.r[i * 3] * v[0] + a.r[i * 3 + 1] * v[1] + a.r[i * 3 + 2] * v[2];
    };
    const auto norm = [](float v[3]) {
        const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (l > 1e-12f)
            for (int k = 0; k < 3; ++k) v[k] /= l;
        return l > 1e-12f;
    };
    // the smallest rotation taking unit a onto unit b (Rodrigues)
    const auto minRot = [&](const float a[3], const float b[3]) {
        const float c = std::clamp(a[0] * b[0] + a[1] * b[1] + a[2] * b[2], -1.0f, 1.0f);
        float k[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
        float s = std::sqrt(k[0] * k[0] + k[1] * k[1] + k[2] * k[2]);
        M3 m;
        if (s < 1e-6f) {
            if (c > 0.0f) return m;
            const float e[3] = {std::fabs(a[0]) < 0.9f ? 1.0f : 0.0f, std::fabs(a[0]) < 0.9f ? 0.0f : 1.0f, 0.0f};
            k[0] = a[1] * e[2] - a[2] * e[1], k[1] = a[2] * e[0] - a[0] * e[2], k[2] = a[0] * e[1] - a[1] * e[0];
            norm(k);
            s = 0.0f;
        } else {
            for (int i = 0; i < 3; ++i) k[i] /= s;
        }
        const float x = k[0], y = k[1], z = k[2], C = 1.0f - c;
        m.r[0] = c + x * x * C, m.r[1] = x * y * C - z * s, m.r[2] = x * z * C + y * s;
        m.r[3] = y * x * C + z * s, m.r[4] = c + y * y * C, m.r[5] = y * z * C - x * s;
        m.r[6] = z * x * C - y * s, m.r[7] = z * y * C + x * s, m.r[8] = c + z * z * C;
        return m;
    };
    // the model's rotation (world <- model), its columns normalised (the scale out)
    M3 R;
    for (int j = 0; j < 3; ++j) {
        float c[3] = {model.m[j * 4], model.m[j * 4 + 1], model.m[j * 4 + 2]};
        norm(c);
        for (int k = 0; k < 3; ++k) R.r[k * 3 + j] = c[k];
    }
    const M3 Rt = tr(R);
    M3 W[4];
    for (int k = 1; k < shape.parts && k < 4; ++k) {
        float seg[3] = {chain[k][0] - chain[k - 1][0], chain[k][1] - chain[k - 1][1], chain[k][2] - chain[k - 1][2]};
        float want[3], cur[3];
        apply(Rt, seg, want);
        apply(W[k - 1], shape.axis[k], cur);
        if (!norm(want) || !norm(cur)) {
            W[k] = W[k - 1];
            continue;
        }
        W[k] = mul(minRot(cur, want), W[k - 1]);
    }
    for (int k = 0; k < 4; ++k) {
        const M3 local = k == 0 || k >= shape.parts ? M3{} : mul(tr(W[k - 1]), W[k]);
        for (int e = 0; e < 9; ++e)
            out[k].m[e] = static_cast<int16_t>(std::lround(std::clamp(local.r[e], -1.0f, 1.0f) * 4096.0f));
    }
}
const WeaponHandOverride& CurrentWeaponHandOverride() { return g_handOverride; }

bool WeaponDraw::Load(const rr::DiscImage& disc, int bank) {
    const int b = (bank < 0 ? 0 : bank > 2 ? 2 : bank) + 1;
    char name[32], texName[32];
    std::snprintf(name, sizeof(name), "DATA/BBLEVEL%d.GEO", b);
    std::snprintf(texName, sizeof(texName), "DATA/BBLEVEL%d.TEX", b);
    const auto geo = disc.Find(name);
    const auto tex = disc.Find(texName);
    const auto overlay = disc.Find("RASHCDG.BIN");
    if (!geo || !overlay) return false;
    std::vector<rr::Model> models;
    try {
        models = rr::ParseGeo(disc.ReadFile(*geo));
        skeleton_ = rr::SkeletonTable::FromOverlay(disc.ReadFile(*overlay));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "weapon models: %s did not parse (%s)\n", name, e.what());
        return false;
    }
    uint32_t sheetId = 0;
    for (const rr::Model& model : models) {
        if (model.id == kRiderModel && !model.groups.empty()) {
            riderGroup_ = model.groups.front();
            riderAssembly_ = rr::AssembleGroup(riderGroup_, skeleton_);
            haveRider_ = riderGroup_.subMeshes.size() == 17;
        }
        if (model.id != kWeaponModel) continue;
        sheetId = rr::ModelTextureId(model);
        for (const rr::ModelGroup& group : model.groups) groups_.push_back({group, rr::AssembleGroup(group, skeleton_)});
    }
    if (groups_.empty() || !haveRider_) return false;
    if (tex) {
        try {
            sheet_ = UploadIndexedTexture(rr::BuildLectAtlas(disc.ReadFile(*tex), static_cast<int>(sheetId), 16));
        } catch (const std::exception& e) {
            std::fprintf(stderr, "weapon sheet: LECT %u of %s: %s\n", sheetId, texName, e.what());
        }
    }
    gl.GenVertexArrays(1, &vao_);
    gl.BindVertexArray(vao_);
    gl.GenBuffers(1, &vbo_);
    gl.BindBuffer(GL_ARRAY_BUFFER, vbo_);
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
    std::printf("weapon models: %s model 800, %zu group(s); sheet LECT %u of %s %s; hands from model 150 program %zu\n",
                name, groups_.size(), sheetId, texName, sheet_.Valid() ? "(16 palettes from its last rows)" : "NOT loaded",
                riderAssembly_.programIndex);
    return true;
}

bool WeaponDraw::Draw(const RaceScene& scene, const Mat4& viewProj, const uint8_t* ram, uint32_t rider,
                      const Mat4& riderModel, const rr::PartMatrix* riderLocal) {
    if (vao_ == 0 || ram == nullptr || rider < 0x80000000u || rider >= 0x80200000u) return false;
    // seat 0 of the rider: a weapon model object, its kind (the hand) and its LOD (the weapon)
    uint32_t obj = Word(ram, rider + 0x38u);
    const bool inTrackedHand = g_handOverride.active && rider == g_handOverride.rider; // VR (WeaponHandOverride)
    if (inTrackedHand && g_handOverride.hidden) return false;
    uint32_t hand = 0;
    int group = -1;
    if (obj < kObjSlots || obj >= kObjSlots + 8u * kObjBytes || (obj - kObjSlots) % kObjBytes != 0) {
        // no weapon object: VR physical combat may still show the weapon in hand in the tracked hand
        if (!inTrackedHand || g_handOverride.unheld < 0) return false;
        obj = 0;
        group = g_handOverride.unheld;
    } else {
        hand = Word(ram, rider + 0x3Cu);
        group = static_cast<int8_t>(ram[(obj + 8u) & 0x1FFFFFu]);
    }
    if (hand >= 17u || group < 0 || static_cast<size_t>(group) >= groups_.size()) return false;
    const Group& wg = groups_[static_cast<size_t>(group)];
    // the hand's frame, the attachment program walked exactly as the rider's own mesh is posed
    std::span<const rr::PartMatrix> local;
    if (riderLocal != nullptr) local = std::span<const rr::PartMatrix>(riderLocal, 17);
    const rr::PosedGroup posed = rr::PoseGroup(riderGroup_, skeleton_, riderAssembly_, local);
    Mat4 frame;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c)
            frame.m[c * 4 + r] = static_cast<float>(posed.world[hand].m[r * 3 + c]) / 4096.0f;
        frame.m[12 + r] = static_cast<float>(posed.origin[hand][static_cast<size_t>(r)]);
    }
    // the weapon's own parts, from the object's part slots (+0x04: 24 bytes each, the 3x3 at +4)
    const size_t parts = wg.group.subMeshes.size();
    std::vector<rr::PartMatrix> slots(parts);
    const uint32_t array = obj != 0 ? Word(ram, obj + 4u) : 0u;
    const bool haveSlots = array >= 0x80000000u && array < 0x80200000u;
    // A jointed weapon in a tracked hand following the chain physics (weapon_draw.h chainCount): the hand
    // frame from the rest pose's long axis, the parts turned onto the simulated segments under that very matrix
    ChainShape shape;
    const bool ourChain = inTrackedHand && g_handOverride.chainCount > 0 && static_cast<size_t>(g_handOverride.chainCount) == parts &&
                          ChainShapeOf(wg.group, skeleton_, wg.assembly, shape);
    rr::TriangleSoup restSoup;
    rr::PartMatrix chainSlots[4];
    if (ourChain) {
        restSoup = rr::BuildPosedTriangleSoup(wg.group, rr::PoseGroup(wg.group, skeleton_, wg.assembly, std::span<const rr::PartMatrix>{}));
        ChainSlots(HandModel(restSoup, g_handOverride), shape, g_handOverride.chain, chainSlots);
    }
    bool rest = true;
    for (size_t k = 0; k < parts; ++k)
        for (uint32_t e = 0; e < 9; ++e) {
            const int16_t id = static_cast<int16_t>(e % 4u == 0u ? 4096 : 0);
            const int16_t v = ourChain ? chainSlots[k].m[e]
                            : haveSlots ? Half(ram, array + 24u * static_cast<uint32_t>(k) + 4u + 2u * e) : id;
            slots[k].m[e] = v;
            if (v != id) rest = false;
        }
    const rr::PosedGroup mine = rr::PoseGroup(wg.group, skeleton_, wg.assembly, std::span<const rr::PartMatrix>(slots));
    const rr::TriangleSoup soup = rr::BuildPosedTriangleSoup(wg.group, mine);
    if (soup.vertices.empty()) return false;
    const GLuint program = scene.Program();
    gl.UseProgram(program);
    UploadViewProj(gl.GetUniformLocation(program, "uViewProj"), viewProj); // both eyes in stereo (multiview.h)
    scene.SetDebug(0);
    scene.SetTint(0.62f, 0.62f, 0.66f); // only where no sheet binds
    if (sheet_.Valid()) {
        scene.SetTextured(1);
        gl.ActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, sheet_.indexTexture);
        gl.Uniform1i(gl.GetUniformLocation(program, "uIndex"), 0);
        gl.ActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, sheet_.paletteTexture);
        gl.Uniform1i(gl.GetUniformLocation(program, "uPalette"), 1);
        gl.ActiveTexture(GL_TEXTURE0);
        gl.Uniform2f(gl.GetUniformLocation(program, "uTexSize"), sheet_.width, sheet_.height);
        gl.Uniform1f(gl.GetUniformLocation(program, "uPaletteCount"), sheet_.paletteCount);
        gl.Uniform1f(gl.GetUniformLocation(program, "uPaletteSize"), sheet_.paletteSize);
        scene.BindSmooth(sheet_); // the PC graphics settings' smooth textures (race_scene_pc.cpp)
    } else {
        scene.SetTextured(0);
    }
    scene.SetModelMatrix(inTrackedHand ? HandModel(ourChain ? restSoup : soup, g_handOverride) : Multiply(riderModel, frame));
    gl.BindVertexArray(vao_);
    gl.BindBuffer(GL_ARRAY_BUFFER, vbo_);
    gl.BufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(soup.vertices.size() * sizeof(rr::TriangleSoup::Vertex)),
                  soup.vertices.data(), GL_DYNAMIC_DRAW);
    {   // the model light (race_scene.h ApplyObjectLight): the weapon object's own +0x24
        ModelPart lp;
        SetPartLight(lp, wg.group);
        scene.ApplyObjectLight(lp.lit, lp.modelClass,
                               RaceScene::LookLightOff() || obj == 0 ? -1 : static_cast<int64_t>(Word(ram, obj + 0x24u)));
    }
    scene.OtSource(vbo_); // the ordering-table order (race_scene_ot.cpp)
    scene.OtObject(obj);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(soup.vertices.size()));
    scene.ClearObjectLight();
    gl.BindVertexArray(0);
    scene.SetModelMatrix(Mat4{});
    scene.SetTextured(0);
    ++drawn_;
    if (!rest) ++posed_;
    return true;
}

// VR (tools\rrgame\vr_holsters.h): the holstered weapons, each group at rest with its own model matrix, drawn
// as Draw draws a weapon (the sheet, the model light without an object).
void WeaponDraw::DrawExtras(const RaceScene& scene, const Mat4& viewProj) {
    if (vao_ == 0 || g_extraWeapons.empty()) return;
    const GLuint program = scene.Program();
    gl.UseProgram(program);
    UploadViewProj(gl.GetUniformLocation(program, "uViewProj"), viewProj);
    scene.SetDebug(0);
    scene.SetTint(0.62f, 0.62f, 0.66f);
    if (sheet_.Valid()) {
        scene.SetTextured(1);
        gl.ActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, sheet_.indexTexture);
        gl.Uniform1i(gl.GetUniformLocation(program, "uIndex"), 0);
        gl.ActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, sheet_.paletteTexture);
        gl.Uniform1i(gl.GetUniformLocation(program, "uPalette"), 1);
        gl.ActiveTexture(GL_TEXTURE0);
        gl.Uniform2f(gl.GetUniformLocation(program, "uTexSize"), sheet_.width, sheet_.height);
        gl.Uniform1f(gl.GetUniformLocation(program, "uPaletteCount"), sheet_.paletteCount);
        gl.Uniform1f(gl.GetUniformLocation(program, "uPaletteSize"), sheet_.paletteSize);
        scene.BindSmooth(sheet_);
    } else {
        scene.SetTextured(0);
    }
    gl.BindVertexArray(vao_);
    gl.BindBuffer(GL_ARRAY_BUFFER, vbo_);
    for (const ExtraWeaponDraw& e : g_extraWeapons) {
        if (e.group < 0 || static_cast<size_t>(e.group) >= groups_.size()) continue;
        const Group& wg = groups_[static_cast<size_t>(e.group)];
        const rr::TriangleSoup soup = rr::BuildPosedTriangleSoup(
            wg.group, rr::PoseGroup(wg.group, skeleton_, wg.assembly, std::span<const rr::PartMatrix>{}));
        if (soup.vertices.empty()) continue;
        scene.SetModelMatrix(e.matrix);
        gl.BufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(soup.vertices.size() * sizeof(rr::TriangleSoup::Vertex)),
                      soup.vertices.data(), GL_DYNAMIC_DRAW);
        ModelPart lp;
        SetPartLight(lp, wg.group);
        scene.ApplyObjectLight(lp.lit, lp.modelClass, -1);
        scene.OtSource(vbo_);
        scene.OtObject(0);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(soup.vertices.size()));
        scene.ClearObjectLight();
        ++extrasDrawn_;
    }
    gl.BindVertexArray(0);
    scene.SetModelMatrix(Mat4{});
    scene.SetTextured(0);
}

} // namespace rr::render
