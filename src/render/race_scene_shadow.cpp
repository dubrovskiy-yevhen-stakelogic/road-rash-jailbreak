// The PORTED model shadow drawn (race_scene.h SetPortedShadows).
//
// The quads come from SLUS 0x80025EE0 run in the product at the tail of the emitter the model runtime serves
// (src\game\sim\shadow.h, src\game\shadow_product.h): every object the original gives a shadow (+0x24 bit 7,
// set by ModelVisible for the one-player race's near machines), its OWN model's quadsD hull at the LOD drawn,
// posed by the PORTED model draw, projected along the level light bent to 45 degrees onto the plane of its ground
// point and normal (+0x1F8 / +0x20A of the bike). Drawn as the GPU draws the packets: flat, mode 2 (B - F) with
// the packet's colour, the mask bit set and tested (a stencil here: a pixel is darkened once however many quads
// cover it), no depth test (the road under it is drawn first and the machine after).
#include "render/race_scene.h"

#include "render/edge_rule.h"
#include "render/ot_order.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rr::render {

void RaceScene::SetPortedShadows(std::vector<PortedShadowQuad> quads, bool active) {
    portedShadows_ = std::move(quads);
    portedShadowsActive_ = active;
    portedShadowsDrawn_ = false;
}

bool RaceScene::DrawPortedShadows(const DrawRequest& request, int debug) {
    if (!portedShadowsActive_) return false;
    if (otOrder_ && !otFlushing_) return true; // drawn after the opaque view at its slots (race_scene_ot.cpp)
    if (portedShadowsDrawn_ || shadowProgram_ == 0) return true;
    portedShadowsDrawn_ = true;
    if (portedShadows_.empty()) return true;
    std::vector<float> tris;
    tris.reserve(portedShadows_.size() * 36);
    // The packet's corner order is (q0, q1, q3, q2), which the GPU splits into (q0, q1, q3) and (q1, q3, q2).
    static constexpr int kOrder[6] = {0, 1, 3, 1, 3, 2};
    // Each corner at the packet's own SXY (the RTPT the port ran), at the view depth the float
    // camera gives its world point; RRJB_GTE_SHADOW=world: the world points through the float camera (the control).
    static const bool worldOnly = std::getenv("RRJB_GTE_SHADOW") != nullptr && std::strcmp(std::getenv("RRJB_GTE_SHADOW"), "world") == 0;
    for (const PortedShadowQuad& q : portedShadows_) {
        for (int c = 0; c < 4; ++c) shadowWorld_.push_back({q.world[c][0], q.world[c][1], q.world[c][2]});
        float at[4][3];
        bool gte = gteOn_ && q.haveSxy && !worldOnly;
        for (int c = 0; c < 4; ++c) {
            for (int k = 0; k < 3; ++k) at[c][k] = q.world[c][k];
            if (!gte) continue;
            const float* m = request.viewProj.m;
            const double w = m[3] * q.world[c][0] + m[7] * q.world[c][1] + m[11] * q.world[c][2] + m[15];
            gte = GteScreenToWorld(q.sxy[c][0], q.sxy[c][1], std::max(w, kGteMinW), at[c]);
        }
        if (!gte)
            for (int c = 0; c < 4; ++c)
                for (int k = 0; k < 3; ++k) at[c][k] = q.world[c][k];
        else
            ++gteShadowQuads_;
        for (int t = 0; t < 6; t += 3) {
            const auto word = [&q](int c) {
                return (static_cast<uint32_t>(static_cast<uint16_t>(q.sxy[c][1])) << 16) | static_cast<uint16_t>(q.sxy[c][0]);
            };
            const bool refused = gte && GpuRejectOn() && GpuRejects(word(kOrder[t]), word(kOrder[t + 1]), word(kOrder[t + 2]));
            if (refused) ++GteRejected(1);
            for (int j = t; j < t + 3; ++j) {
                const int c = refused ? kOrder[t] : kOrder[j];
                for (int k = 0; k < 3; ++k) tris.push_back(at[c][k]);
                // the fill rule (edge_rule.h): the packet's own SXY, where the vertex stage puts the corner exactly
                // (the world point's float transform lands up to ~0.2 px off it) and the console's fill rule is taken
                tris.push_back(static_cast<float>(q.sxy[c][0]));
                tris.push_back(static_cast<float>(q.sxy[c][1]));
                tris.push_back(gte && EdgeRuleOn() ? 1.0f : 0.0f);
            }
        }
    }
    const float* colour = portedShadows_.front().colour; // 0x80052348: one colour for every packet
    gl.UseProgram(shadowProgram_);
    UploadViewProj(shadowViewProjLocation_, request.viewProj);
    gl.Uniform3f(shadowColourLocation_, colour[0], colour[1], colour[2]);
    gl.Uniform1i(shadowDebugLocation_, debug);
    // the fill rule (edge_rule.h): the corners at their packets' SXY (attribute 1) are console points
    gl.Uniform4f(gl.GetUniformLocation(shadowProgram_, "uGteMap"), gteMap_[0], gteMap_[1], gteMap_[2], gteMap_[3]);
    gl.Uniform1f(gl.GetUniformLocation(shadowProgram_, "uGtePix"), gtePix_);
    EdgeViewport(shadowProgram_);
    // Its own vertex array over the shadow buffer (the approximate DrawShadow keeps positions only): the world point, then
    // the packet's SXY and whether the corner sits there (edge_rule.h). One GL context per process.
    static GLuint portedVao = 0;
    if (portedVao == 0) {
        gl.GenVertexArrays(1, &portedVao);
        gl.BindVertexArray(portedVao);
        gl.BindBuffer(GL_ARRAY_BUFFER, shadowVbo_);
        constexpr GLsizei kStride = 6 * static_cast<GLsizei>(sizeof(float));
        gl.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, kStride, reinterpret_cast<void*>(0));
        gl.EnableVertexAttribArray(0);
        gl.VertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, kStride, reinterpret_cast<void*>(3 * sizeof(float)));
        gl.EnableVertexAttribArray(1);
    }
    gl.BindVertexArray(portedVao);
    gl.BindBuffer(GL_ARRAY_BUFFER, shadowVbo_);
    gl.BufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(tris.size() * sizeof(float)), tris.data(), GL_DYNAMIC_DRAW);
    glDepthMask(GL_FALSE);
    // The ordering-table order (race_scene_ot.cpp): each packet at the slot the port linked it into, tested against
    // the opaque world's slots (not written) - what is linked nearer covers it, what is linked farther lies under it.
    // Otherwise no depth test.
    if (otOrder_) glEnable(GL_DEPTH_TEST);
    else glDisable(GL_DEPTH_TEST);
    gl.Uniform1i(gl.GetUniformLocation(shadowProgram_, "uOtOrder"), otOrder_ ? 1 : 0);
    const GLint otDepthLocation = gl.GetUniformLocation(shadowProgram_, "uOtDepth");
    glDisable(GL_CULL_FACE);
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_EQUAL, 0, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
    if (debug == 0) {
        glEnable(GL_BLEND);
        gl.BlendEquation(GL_FUNC_REVERSE_SUBTRACT);
        glBlendFunc(GL_ONE, GL_ONE);
    }
    if (otOrder_) {
        for (size_t i = 0; i < portedShadows_.size(); ++i) {
            const OtPassMap& map = OtPass(OtPassOfObject(portedShadows_[i].object)); // its object's table
            const int slot = portedShadows_[i].slot >= 0 ? std::min(portedShadows_[i].slot, map.maxSlot) : map.maxSlot;
            gl.Uniform1f(otDepthLocation, OtDepthOf(slot, 1, map.maxSlot, map.base)); // after the model's own polygons
            glDrawArrays(GL_TRIANGLES, static_cast<GLint>(6 * i), 6);
        }
        otShadowQuads_ += portedShadows_.size();
    } else {
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(tris.size() / 6));
    }
    glDisable(GL_BLEND);
    gl.BlendEquation(GL_FUNC_ADD);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    gl.UseProgram(program_);
    ++portedShadowDraws_;
    return true;
}

} // namespace rr::render

// ------------------------------------------------------------------ the model light of the other objects
// SLUS 0x800251E4 colours EVERY model it emits the same way: a group with a normal
// array and a vertex -> normal table, drawn with game_state+4 bit 4 clear and +0x24 bit 11 clear, gets the per-normal
// ramp colour; any other the unlit step *(0x80052380) (+ 8 for class 3), halved by +0x24 bits 5..6. The cars, the
// pedestrians, the props, the weapons and the sidecar rig go through it like the machines do.
namespace rr::render {

void RaceScene::ApplyObjectLight(bool lit, int modelClass, int64_t objFlags) const {
    ModelPart part;
    part.lit = lit;
    part.modelClass = modelClass;
    ApplyModelLight(part, objFlags);
}

void RaceScene::ClearObjectLight() const { gl.Uniform1i(modelLightLocation_, 0); }

} // namespace rr::render

namespace rr::render {
bool RaceScene::LookLightOff() {
    static const bool off = std::getenv("RRJB_LOOK_LIGHT") != nullptr && std::strcmp(std::getenv("RRJB_LOOK_LIGHT"), "off") == 0;
    return off;
}
} // namespace rr::render

// ------------------------------------------------------------------ the machines from the ported model draw
// The PORTED ModelDraw RASHCDG 0x80068468 (bench-exact) already runs on every visible machine in the
// product, before the effect pass (model_runtime.h), and its camera-space vertex buffer *(0x8005ACB0) is what the
// original's emitter builds the machines' packets from: in rr-race, quick and rr-pack every vertex of every
// machine / rider packet (pages 26 / 27 / 28: 176, 282, 180 packets) is exactly one of its projected points. The
// renderer's own posing of the same objects (rider_pose_draw.h, PosedRiderMatrix, lod_pose_draw.h) is not - the
// head and shoulders and the rr-pack bike showed it. So each group of a machine the model draw drew at the LOD the
// renderer draws is rewritten from those vertices, in the model space of the matrix the draw loop uses; nothing
// else changes (texels, palettes, the model light). RRJB_POSE_FROM=ours: the renderer's posing (the control).
namespace rr::render {
namespace {

bool CapturedToModel(const Mat4& m, const CapturedVerts& c, std::vector<float>& out) {
    double a[9];
    for (int col = 0; col < 3; ++col)
        for (int row = 0; row < 3; ++row) a[3 * row + col] = m.m[4 * col + row];
    const double det = a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6]) +
                       a[2] * (a[3] * a[7] - a[4] * a[6]);
    if (det > -1e-20 && det < 1e-20) return false;
    const double inv[9] = {(a[4] * a[8] - a[5] * a[7]) / det, (a[2] * a[7] - a[1] * a[8]) / det,
                           (a[1] * a[5] - a[2] * a[4]) / det, (a[5] * a[6] - a[3] * a[8]) / det,
                           (a[0] * a[8] - a[2] * a[6]) / det, (a[2] * a[3] - a[0] * a[5]) / det,
                           (a[3] * a[7] - a[4] * a[6]) / det, (a[1] * a[6] - a[0] * a[7]) / det,
                           (a[0] * a[4] - a[1] * a[3]) / det};
    const double d[3] = {c.eye[0] - m.m[12], c.eye[1] - m.m[13], c.eye[2] - m.m[14]};
    out.resize(c.rel.size());
    for (size_t v = 0; v + 2 < c.rel.size(); v += 3) {
        const double w[3] = {d[0] + c.rel[v], d[1] + c.rel[v + 1], d[2] + c.rel[v + 2]};
        for (int r = 0; r < 3; ++r)
            out[v + static_cast<size_t>(r)] = static_cast<float>(inv[3 * r] * w[0] + inv[3 * r + 1] * w[1] + inv[3 * r + 2] * w[2]);
    }
    return true;
}

bool PoseFromOurs() {
    static const bool ours = std::getenv("RRJB_POSE_FROM") != nullptr && std::strcmp(std::getenv("RRJB_POSE_FROM"), "ours") == 0;
    return ours;
}

} // namespace

bool CapturedToModelPoints(const Mat4& m, const CapturedVerts& c, std::vector<float>& out) { return CapturedToModel(m, c, out); }

bool RaceScene::UploadCapturedGroup(const CapturedVerts* c, uint32_t id, const Mat4& m, RiderPoseMesh* m0,
                                    LodPoseMesh* mL, GLuint vbo, int lod) {
    if (c == nullptr || PoseFromOurs() || c->model != id || c->lod != lod) return false;
    std::vector<float> pos;
    if (!CapturedToModel(m, *c, pos)) return false;
    GteModelPoints(*c, m, pos); // gte_proj.cpp: at the model draw's own SXY
    // the GPU's large-polygon rule on the model draw's SXY (gte_proj.h GpuRejects)
    const std::vector<uint32_t>* sxy = gteOn_ && GpuRejectOn() && c->sxy.size() * 3 == pos.size() ? &c->sxy : nullptr;
    return lod == 0 ? (m0 != nullptr && UploadCapturedPose(*m0, vbo, pos, sxy))
                    : (mL != nullptr && UploadCapturedLod(*mL, vbo, pos, sxy));
}

void RaceScene::ApplyCaptured(const DrawRequest& request, uint32_t bikeId, uint32_t riderId, RiderPoseMesh* bike0,
                              LodPoseMesh* bikeL, RiderPoseMesh* rider0, LodPoseMesh* riderL, GLuint vbo, int bikeLod,
                              int riderLod) {
    // The emitter's back-face test SLUS 0x800251E4 (NCLIP over i0 i1 i2, a one-sided primitive dropped when
    // MAC0 < 0) holds for the machines as for the props (shaders.cpp uNclip): the machine and rider matrices are
    // rotations like the props', so the same sign. RRJB_MACHINE_NCLIP=off: both sides drawn (the control).
    static const bool nclipOff = std::getenv("RRJB_MACHINE_NCLIP") != nullptr;
    gl.Uniform1i(nclipLocation_, nclipOff ? 0 : kPropNclip);
    if (PoseFromOurs()) return;
    const auto one = [&](const CapturedVerts* c, uint32_t id, const Mat4& m, RiderPoseMesh* m0, LodPoseMesh* mL, int lod) {
        if (c == nullptr) return;
        ++(UploadCapturedGroup(c, id, m, m0, mL, vbo, lod) ? capturedDraws_ : capturedMissed_);
    };
    if (!request.sidecar) one(request.bikeCaptured, bikeId, request.bikeModel, bike0, bikeL, bikeLod);
    one(request.riderCaptured, riderId, request.riderModel, rider0, riderL, riderLod);
}

} // namespace rr::render
