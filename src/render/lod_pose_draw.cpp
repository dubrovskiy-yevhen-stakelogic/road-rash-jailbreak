#include "render/lod_pose_draw.h"
#include "render/gte_proj.h"

#include <cstring>
#include <vector>

namespace rr::render {

struct LodPoseMesh {
    rr::ModelGroup group;
    rr::SkeletonTable skeleton;
    int programIndex = -1;
    size_t firstVertex = 0;
    size_t vertexCount = 0;
    std::vector<rr::PartMatrix> last;
    bool uploaded = false;
};

namespace {

using PFNGLBUFFERSUBDATA = void(APIENTRY*)(GLenum, GLsizeiptr, GLsizeiptr, const void*);

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

int LodProgram(LodOwner owner, size_t parts) {
    if (owner == LodOwner::Rider) { // RASHCDG 0x8006745C
        if (parts == 12) return 1;
        if (parts == 4) return 2;
        if (parts == 17) return 0;
        return -1;
    }
    if (parts >= 5) return parts < 6 ? 4 : 3; // RASHCDG 0x80066EC4
    return parts == 1 ? -1 : 5;
}

std::shared_ptr<LodPoseMesh> MakeLodPoseMesh(const rr::ModelGroup& group, const rr::SkeletonTable& skeleton,
                                             LodOwner owner, size_t firstVertex) {
    auto mesh = std::make_shared<LodPoseMesh>();
    mesh->group = group;
    mesh->skeleton = skeleton;
    mesh->programIndex = LodProgram(owner, group.subMeshes.size());
    mesh->firstVertex = firstVertex;
    mesh->vertexCount = PosedLodSoup(*mesh, nullptr, 0).vertices.size();
    return mesh;
}

// RASHCDG 0x80067064's walk over the object's slots: part 0 turned by slot 0; part k (k >= 1) by the
// parent part's world x slot[link.matrixPart], placed at the parent's origin + world(parent) x the
// parent's vertex vertBase + link.vertexOffset.
rr::TriangleSoup PosedLodSoup(const LodPoseMesh& mesh, const rr::PartMatrix* slots, size_t count) {
    const rr::ModelGroup& group = mesh.group;
    const size_t parts = group.subMeshes.size();
    rr::PosedGroup posed;
    posed.world.assign(parts, rr::PartMatrix{});
    posed.origin.assign(parts, {0, 0, 0});
    const auto slot = [&](size_t i) { return (slots == nullptr || i >= count) ? rr::PartMatrix{} : slots[i]; };
    if (parts == 0) return rr::BuildPosedTriangleSoup(group, posed);
    posed.world[0] = slot(0);
    const int factor = rr::LodFactor(group);
    const rr::AttachProgram* program =
        (mesh.programIndex >= 0 && static_cast<size_t>(mesh.programIndex) < mesh.skeleton.Programs().size())
            ? &mesh.skeleton.Programs()[static_cast<size_t>(mesh.programIndex)]
            : nullptr;
    for (size_t child = 1; child < parts; ++child) {
        size_t parent = 0;
        size_t matrix = child;
        std::array<int32_t, 3> offset{0, 0, 0};
        if (program != nullptr && child - 1 < program->links.size()) {
            const rr::AttachLink& link = program->links[child - 1];
            parent = link.parent < parts ? link.parent : 0;
            matrix = link.matrixPart;
            const size_t index = group.subMeshes[parent].vertBase + link.vertexOffset;
            if (index < group.verts.size()) {
                const rr::SVector& v = group.verts[index];
                offset = {v.x * factor, v.y * factor, v.z * factor};
            }
        }
        const rr::PartMatrix& pw = posed.world[parent];
        for (int r = 0; r < 3; ++r) {
            int64_t sum = 0;
            for (int c = 0; c < 3; ++c) sum += static_cast<int64_t>(pw.m[r * 3 + c]) * offset[static_cast<size_t>(c)];
            posed.origin[child][static_cast<size_t>(r)] =
                posed.origin[parent][static_cast<size_t>(r)] + static_cast<int32_t>(sum >> 12);
        }
        posed.world[child] = rr::Multiply3x3(pw, slot(matrix));
    }
    return rr::BuildPosedTriangleSoup(group, posed);
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

bool UploadCapturedLod(LodPoseMesh& mesh, GLuint vbo, const std::vector<float>& modelPos, const std::vector<uint32_t>* sxy) {
    rr::TriangleSoup soup;
    if (!CapturedSoup(mesh.group, modelPos, soup, sxy) || soup.vertices.size() != mesh.vertexCount) return false;
    PFNGLBUFFERSUBDATA sub = BufferSubData();
    if (sub == nullptr) return false;
    gl.BindBuffer(GL_ARRAY_BUFFER, vbo);
    sub(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(mesh.firstVertex * sizeof(rr::TriangleSoup::Vertex)),
        static_cast<GLsizeiptr>(soup.vertices.size() * sizeof(rr::TriangleSoup::Vertex)), soup.vertices.data());
    mesh.uploaded = false; // the next UploadLodPose writes again
    return true;
}

void UploadLodPose(LodPoseMesh& mesh, GLuint vbo, const rr::PartMatrix* slots, size_t count) {
    if (mesh.uploaded && slots != nullptr && mesh.last.size() == count &&
        std::memcmp(mesh.last.data(), slots, sizeof(rr::PartMatrix) * count) == 0)
        return;
    if (mesh.uploaded && slots == nullptr && mesh.last.empty()) return;
    PFNGLBUFFERSUBDATA sub = BufferSubData();
    if (sub == nullptr) return;
    const rr::TriangleSoup soup = PosedLodSoup(mesh, slots, count);
    if (soup.vertices.size() != mesh.vertexCount) return;
    if (slots != nullptr) mesh.last.assign(slots, slots + count);
    else mesh.last.clear();
    mesh.uploaded = true;
    gl.BindBuffer(GL_ARRAY_BUFFER, vbo);
    sub(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(mesh.firstVertex * sizeof(rr::TriangleSoup::Vertex)),
        static_cast<GLsizeiptr>(soup.vertices.size() * sizeof(rr::TriangleSoup::Vertex)), soup.vertices.data());
}

} // namespace rr::render
