#include "render/rider_pose_draw.h"
#include "render/gte_proj.h"

#include <cstring>
#include <span>
#include <vector>

namespace rr::render {

struct RiderPoseMesh {
    rr::ModelGroup group;
    rr::SkeletonTable skeleton;
    rr::Assembly assembly;
    size_t firstVertex = 0;
    size_t vertexCount = 0;
    bool holdsRest = true;
    rr::PartMatrix last[17];
    bool haveLast = false;
};

namespace {

using PFNGLBUFFERSUBDATA = void(APIENTRY*)(GLenum, GLsizeiptr, GLsizeiptr, const void*);

// glBufferSubData (GL 1.5), loaded here so the shared `Gl` table stays as it is.
PFNGLBUFFERSUBDATA BufferSubData() {
    static PFNGLBUFFERSUBDATA fn = [] {
        void* p = reinterpret_cast<void*>(wglGetProcAddress("glBufferSubData"));
        if (p == nullptr || p == reinterpret_cast<void*>(1) || p == reinterpret_cast<void*>(2) ||
            p == reinterpret_cast<void*>(3) || p == reinterpret_cast<void*>(-1)) {
            HMODULE module = GetModuleHandleA("opengl32.dll");
            p = module ? reinterpret_cast<void*>(GetProcAddress(module, "glBufferSubData")) : nullptr;
        }
        return reinterpret_cast<PFNGLBUFFERSUBDATA>(p);
    }();
    return fn;
}

} // namespace

std::shared_ptr<RiderPoseMesh> MakeRiderPoseMesh(const rr::ModelGroup& group, const rr::SkeletonTable& skeleton,
                                                 size_t firstVertex) {
    auto mesh = std::make_shared<RiderPoseMesh>(RiderPoseMesh{group, skeleton, rr::AssembleGroup(group, skeleton),
                                                              firstVertex, 0, true, {}, false});
    mesh->vertexCount = rr::BuildAssembledTriangleSoup(group, mesh->assembly).vertices.size();
    return mesh;
}

void UploadRiderPose(RiderPoseMesh& mesh, GLuint vbo, const rr::PartMatrix* local) {
    const size_t parts = mesh.group.subMeshes.size();
    if (local == nullptr) {
        if (mesh.holdsRest) return;
    } else if (mesh.haveLast && parts <= 17 && std::memcmp(mesh.last, local, sizeof(rr::PartMatrix) * parts) == 0) {
        return;
    }
    PFNGLBUFFERSUBDATA sub = BufferSubData();
    if (sub == nullptr) return;
    rr::TriangleSoup soup;
    if (local == nullptr || parts > 17) {
        soup = rr::BuildAssembledTriangleSoup(mesh.group, mesh.assembly);
        mesh.holdsRest = true;
        mesh.haveLast = false;
    } else {
        soup = rr::BuildPosedTriangleSoup(
            mesh.group, rr::PoseGroup(mesh.group, mesh.skeleton, mesh.assembly, std::span<const rr::PartMatrix>(local, parts)));
        std::memcpy(mesh.last, local, sizeof(rr::PartMatrix) * parts);
        mesh.haveLast = true;
        mesh.holdsRest = false;
    }
    if (soup.vertices.size() != mesh.vertexCount) return;
    gl.BindBuffer(GL_ARRAY_BUFFER, vbo);
    sub(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(mesh.firstVertex * sizeof(rr::TriangleSoup::Vertex)),
        static_cast<GLsizeiptr>(soup.vertices.size() * sizeof(rr::TriangleSoup::Vertex)), soup.vertices.data());
}

// The group's triangles (BuildTriangleSoup's order, rmd3.h SoupCorners) with every corner at the
// position `modelPos` (3 floats per model vertex, in the group's model space) gives its vertex.
static bool CapturedSoup(const rr::ModelGroup& group, const std::vector<float>& modelPos, rr::TriangleSoup& soup,
                         const std::vector<uint32_t>* sxy) {
    if (modelPos.size() != 3 * group.verts.size()) return false;
    soup = rr::BuildTriangleSoup(group);
    size_t at = 0;
    for (const rr::SubMesh& sub : group.subMeshes)
        for (const rr::Primitive& prim : sub.prims) {
            uint32_t corner[6] = {};
            for (int k = 0; k < 6; ++k) {
                const size_t vi = prim.index[static_cast<size_t>(rr::SoupCorners(prim)[k])];
                if (at >= soup.vertices.size() || vi >= group.verts.size()) return false;
                rr::TriangleSoup::Vertex& v = soup.vertices[at++];
                v.x = modelPos[3 * vi];
                v.y = modelPos[3 * vi + 1];
                v.z = modelPos[3 * vi + 2];
                if (sxy != nullptr && vi < sxy->size()) corner[k] = (*sxy)[vi];
            }
            // gte_proj.h: a triangle the GPU refuses at the model draw's SXY is drawn as nothing
            for (int t = 0; t < 6 && sxy != nullptr; t += 3)
                if (GpuRejects(corner[t], corner[t + 1], corner[t + 2])) {
                    CollapseTriangle(&soup.vertices[at - 6 + static_cast<size_t>(t)]);
                    ++GteRejected(1);
                }
        }
    return at == soup.vertices.size();
}

bool UploadCapturedPose(RiderPoseMesh& mesh, GLuint vbo, const std::vector<float>& modelPos, const std::vector<uint32_t>* sxy) {
    rr::TriangleSoup soup;
    if (!CapturedSoup(mesh.group, modelPos, soup, sxy) || soup.vertices.size() != mesh.vertexCount) return false;
    PFNGLBUFFERSUBDATA sub = BufferSubData();
    if (sub == nullptr) return false;
    gl.BindBuffer(GL_ARRAY_BUFFER, vbo);
    sub(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(mesh.firstVertex * sizeof(rr::TriangleSoup::Vertex)),
        static_cast<GLsizeiptr>(soup.vertices.size() * sizeof(rr::TriangleSoup::Vertex)), soup.vertices.data());
    mesh.holdsRest = false; // the buffer holds neither the rest pose nor `last` now
    mesh.haveLast = false;
    return true;
}

Mat4 PosedRiderMatrix(const Mat4& bike, const float attach[3], const int16_t root[3]) {
    Mat4 rider = bike;
    const float p[3] = {attach[0] + static_cast<float>(root[0]), attach[1] + static_cast<float>(root[1]),
                        attach[2] + static_cast<float>(root[2])};
    for (int k = 0; k < 3; ++k)
        rider.m[12 + k] = bike.m[12 + k] + bike.m[0 + k] * p[0] + bike.m[4 + k] * p[1] + bike.m[8 + k] * p[2];
    return rider;
}

Mat4 TurnedBySlot(const Mat4& m, const rr::PartMatrix& s) {
    Mat4 out = m;
    for (int a = 0; a < 3; ++a)
        for (int k = 0; k < 3; ++k) {
            float v = 0.0f;
            for (int r = 0; r < 3; ++r) v += static_cast<float>(s.At(r, a)) / 4096.0f * m.m[4 * r + k];
            out.m[4 * a + k] = v;
        }
    return out;
}

Mat4 ChildRiderMatrix(const Mat4& bike, const float attach[3], const int16_t root[3], const rr::PartMatrix& slot2) {
    Mat4 rider = bike;
    float p[3];
    for (int r = 0; r < 3; ++r) {
        float t = 0.0f;
        for (int c = 0; c < 3; ++c) t += static_cast<float>(slot2.At(r, c)) / 4096.0f * static_cast<float>(root[c]);
        p[r] = attach[r] + t;
    }
    for (int k = 0; k < 3; ++k)
        rider.m[12 + k] = bike.m[12 + k] + bike.m[0 + k] * p[0] + bike.m[4 + k] * p[1] + bike.m[8 + k] * p[2];
    return rider;
}

Mat4 OwnRiderMatrix(const float axis[3][3], const float origin[3]) {
    Mat4 m;
    for (int c = 0; c < 3; ++c) {
        for (int k = 0; k < 3; ++k) m.m[4 * c + k] = axis[c][k] / 1024.0f; // race_scene.h kModelUnitsPerWorldUnit
        m.m[4 * c + 3] = 0.0f;
    }
    for (int k = 0; k < 3; ++k) m.m[12 + k] = origin[k];
    m.m[15] = 1.0f;
    return m;
}

} // namespace rr::render
