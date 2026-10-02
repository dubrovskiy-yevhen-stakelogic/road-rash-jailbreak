#include "rrformats/rmd3.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace rr {
namespace {

constexpr uint32_t kChunkHeader = 8; // tag + size

uint16_t ReadU16(std::span<const uint8_t> d, size_t off) {
    if (off + 2 > d.size()) throw std::runtime_error("RMD3: read past end of file");
    return static_cast<uint16_t>(d[off] | (d[off + 1] << 8));
}

int16_t ReadS16(std::span<const uint8_t> d, size_t off) {
    return static_cast<int16_t>(ReadU16(d, off));
}

uint32_t ReadU32(std::span<const uint8_t> d, size_t off) {
    if (off + 4 > d.size()) throw std::runtime_error("RMD3: read past end of file");
    return static_cast<uint32_t>(d[off]) | (static_cast<uint32_t>(d[off + 1]) << 8) |
           (static_cast<uint32_t>(d[off + 2]) << 16) | (static_cast<uint32_t>(d[off + 3]) << 24);
}

bool TagIs(std::span<const uint8_t> d, size_t off, const char* tag) {
    if (off + 4 > d.size()) return false;
    return std::memcmp(d.data() + off, tag, 4) == 0;
}

void Expect(bool condition, const char* what) {
    if (!condition) throw std::runtime_error(std::string("RMD3: ") + what);
}

SVector ReadSVector(std::span<const uint8_t> d, size_t off) {
    SVector v;
    v.x = ReadS16(d, off + 0);
    v.y = ReadS16(d, off + 2);
    v.z = ReadS16(d, off + 4);
    v.pad = ReadS16(d, off + 6);
    return v;
}

// A counted region: u32 count followed by count*stride bytes. Returns the count; `at` is
// advanced past the payload. Absent regions are encoded as offset 0.
uint32_t ReadCount(std::span<const uint8_t> d, size_t regionOffset) {
    return ReadU32(d, regionOffset);
}

ModelGroup ParseGroup(std::span<const uint8_t> d, size_t& at, size_t objectEnd, uint32_t modelId) {
    Expect(TagIs(d, at, "DOD3"), "expected DOD3");
    const size_t dod = at;
    const uint32_t dodSize = ReadU32(d, dod + 0x04);
    Expect(dodSize >= 0x34 && dod + dodSize <= objectEnd, "DOD3 size out of range");
    Expect(ReadU32(d, dod + 0x08) == modelId, "DOD3 model id does not match its RMD3");

    ModelGroup g;
    g.flags = ReadU32(d, dod + 0x0C);
    g.radius = ReadU32(d, dod + 0x10);
    g.scale = ReadU32(d, dod + 0x14);
    const uint32_t subMeshCount = ReadU32(d, dod + 0x18);
    g.slot = ReadU32(d, dod + 0x1C);

    const uint32_t ofsBlobA = ReadU32(d, dod + 0x20);
    const uint32_t ofsVerts = ReadU32(d, dod + 0x24);
    const uint32_t ofsNormals = ReadU32(d, dod + 0x28);
    const uint32_t ofsQuadsD = ReadU32(d, dod + 0x2C);
    const uint32_t ofsVertNormIdx = ReadU32(d, dod + 0x30);

    if (ofsBlobA) {
        Expect(ofsBlobA == 0x34, "blobA does not start at the end of the DOD3 header");
        const uint32_t end = ofsVerts ? ofsVerts : dodSize;
        Expect(end >= ofsBlobA, "blobA region runs backwards");
        g.blobA.assign(d.begin() + static_cast<ptrdiff_t>(dod + ofsBlobA),
                       d.begin() + static_cast<ptrdiff_t>(dod + end));
    }
    if (ofsVerts) {
        const uint32_t count = ReadCount(d, dod + ofsVerts);
        g.verts.reserve(count);
        for (uint32_t i = 0; i < count; ++i) g.verts.push_back(ReadSVector(d, dod + ofsVerts + 4 + i * 8));
    }
    if (ofsNormals) {
        const uint32_t count = ReadCount(d, dod + ofsNormals);
        g.normals.reserve(count);
        for (uint32_t i = 0; i < count; ++i) g.normals.push_back(ReadSVector(d, dod + ofsNormals + 4 + i * 8));
    }
    if (ofsQuadsD) {
        const uint32_t count = ReadCount(d, dod + ofsQuadsD);
        g.quadsD.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            std::array<uint16_t, 4> q{};
            for (int k = 0; k < 4; ++k) q[static_cast<size_t>(k)] = ReadU16(d, dod + ofsQuadsD + 4 + i * 8 + static_cast<size_t>(k) * 2);
            g.quadsD.push_back(q);
        }
    }
    if (ofsVertNormIdx) {
        const uint32_t count = ReadCount(d, dod + ofsVertNormIdx);
        g.vertNormalIndex.reserve(count);
        for (uint32_t i = 0; i < count; ++i)
            g.vertNormalIndex.push_back(ReadU16(d, dod + ofsVertNormIdx + 4 + i * 2));
    }

    at = dod + dodSize;

    uint32_t runningBase = 0;
    for (uint32_t s = 0; s < subMeshCount; ++s) {
        Expect(TagIs(d, at, "DPD3"), "expected DPD3");
        const size_t dpd = at;
        const uint32_t dpdSize = ReadU32(d, dpd + 0x04);
        Expect(dpdSize >= 0x1C && dpd + dpdSize <= objectEnd, "DPD3 size out of range");
        Expect(ReadU32(d, dpd + 0x08) == modelId, "DPD3 model id does not match its RMD3");
        Expect(d[dpd + 0x0D] == s, "DPD3 sub-mesh index is out of order");

        SubMesh sub;
        sub.vertCount = ReadU16(d, dpd + 0x0E);
        sub.vertBase = ReadU32(d, dpd + 0x10);
        Expect(sub.vertBase == runningBase, "DPD3 vertBase is not the running prefix sum");
        runningBase += sub.vertCount;
        Expect(ReadU32(d, dpd + 0x14) == 0x18, "DPD3 count offset is not 0x18");

        const uint32_t primCount = ReadU32(d, dpd + 0x18);
        Expect(0x1C + 20u * primCount == dpdSize, "DPD3 primitive count does not fill the chunk");
        sub.prims.reserve(primCount);
        for (uint32_t p = 0; p < primCount; ++p) {
            const size_t o = dpd + 0x1C + p * 20;
            Primitive prim;
            // The texel of corner i0 is the pair at +0x04 and that of i1 the pair at +0x00: the model
            // emitter SLUS 0x800251E4 builds its GT4/FT4 as (i0, uv +4), (i1, uv +0), (i3, uv +10),
            // (i2, uv +8) (0x80025A08..0x80025A68; the NCLIP at 0x8002595C takes i0 / i1 / i2 from the
            // index halfwords +0x0C / +0x0E / +0x10). Measured against the original's packets: of the
            // rr-race / rr-pack model packets whose texel shape names one primitive, 181 carry this
            // association and none the disc order (rrview --propcheck checks it on the props).
            prim.u[1] = d[o + 0x00];
            prim.v[1] = d[o + 0x01];
            prim.clut = ReadU16(d, o + 0x02);
            prim.u[0] = d[o + 0x04];
            prim.v[0] = d[o + 0x05];
            prim.tpage = ReadU16(d, o + 0x06);
            prim.u[2] = d[o + 0x08];
            prim.v[2] = d[o + 0x09];
            prim.u[3] = d[o + 0x0A];
            prim.v[3] = d[o + 0x0B];
            for (int k = 0; k < 4; ++k) {
                const uint16_t index = ReadU16(d, o + 0x0C + static_cast<size_t>(k) * 2);
                Expect(index < g.verts.size(), "primitive vertex index out of range");
                prim.index[static_cast<size_t>(k)] = index;
            }
            sub.prims.push_back(prim);
        }
        g.subMeshes.push_back(std::move(sub));
        at = dpd + dpdSize;
    }
    Expect(runningBase == g.verts.size(), "sub-meshes do not partition the vertex array");

    Expect(TagIs(d, at, "BBD3"), "expected BBD3");
    const size_t bb = at;
    const uint32_t bbSize = ReadU32(d, bb + 0x04);
    Expect(bbSize == 0x20, "BBD3 is not 0x20 bytes");
    Expect(ReadU32(d, bb + 0x08) == modelId, "BBD3 model id does not match its RMD3");
    g.bbox.centre = ReadSVector(d, bb + 0x10);
    for (int k = 0; k < 3; ++k) g.bbox.half[k] = ReadS16(d, bb + 0x18 + static_cast<size_t>(k) * 2);
    g.bbox.radius = ReadS16(d, bb + 0x1E);
    at = bb + bbSize;

    return g;
}

} // namespace

std::vector<Model> ParseGeo(std::span<const uint8_t> data) {
    std::vector<Model> models;
    size_t at = 0;
    while (at < data.size()) {
        Expect(TagIs(data, at, "RMD3"), "expected RMD3 at the start of an object");
        const size_t object = at;
        const uint32_t size = ReadU32(data, object + 0x04);
        Expect(size >= 0x10 && object + size <= data.size(), "RMD3 size runs past end of file");
        const size_t objectEnd = object + size;

        Model model;
        model.id = ReadU32(data, object + 0x08);
        const uint8_t groupCount = data[object + 0x0C];

        size_t cursor = object + 0x10;
        for (uint8_t i = 0; i < groupCount; ++i)
            model.groups.push_back(ParseGroup(data, cursor, objectEnd, model.id));
        Expect(cursor == objectEnd, "groups do not fill the RMD3 chunk exactly");

        models.push_back(std::move(model));
        at = objectEnd;
    }
    Expect(!models.empty(), "no RMD3 chunk in file");
    return models;
}

std::vector<Model> ParseMro(std::span<const uint8_t> data) {
    std::vector<Model> models;
    size_t at = 0;
    while (at < data.size()) {
        Expect(at + 8 <= data.size(), "MRO chunk header runs past end of file");
        const uint32_t size = ReadU32(data, at + 0x04);
        Expect(size >= 8 && at + size <= data.size(), "MRO chunk size runs past end of file");
        if (TagIs(data, at, "RMD3"))
            for (Model& m : ParseGeo(data.subspan(at, size))) models.push_back(std::move(m));
        at += size;
    }
    Expect(!models.empty(), "no RMD3 chunk in MRO file");
    return models;
}

int LodFactor(const ModelGroup& group) {
    const uint32_t exponent = (group.flags >> 16) >> 12;
    const int shift = 4 - static_cast<int>(exponent);
    return shift > 0 ? (1 << shift) : 1;
}

namespace {

// Origins for one candidate program. Returns false if a link points outside the model.
bool OriginsFor(const ModelGroup& group, const AttachProgram& program, std::vector<SVector>& origins) {
    origins.assign(group.subMeshes.size(), SVector{});
    for (size_t i = 0; i < program.links.size(); ++i) {
        const size_t child = i + 1;
        const size_t parent = program.links[i].parent;
        if (child >= origins.size() || parent >= group.subMeshes.size()) return false;
        const size_t vertex = group.subMeshes[parent].vertBase + program.links[i].vertexOffset;
        if (vertex >= group.verts.size()) return false;
        const SVector& offset = group.verts[vertex];
        origins[child].x = static_cast<int16_t>(origins[parent].x + offset.x);
        origins[child].y = static_cast<int16_t>(origins[parent].y + offset.y);
        origins[child].z = static_cast<int16_t>(origins[parent].z + offset.z);
    }
    return true;
}

// Largest deviation between the assembled bounding box and the authored BBD3 box.
int32_t BoxError(const ModelGroup& group, const std::vector<SVector>& origins) {
    bool any = false;
    int32_t lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
    for (const SubMesh& sub : group.subMeshes) {
        const size_t part = static_cast<size_t>(&sub - group.subMeshes.data());
        for (const Primitive& prim : sub.prims)
            for (uint16_t index : prim.index) {
                const SVector& v = group.verts[index];
                const int32_t p[3] = {v.x + origins[part].x, v.y + origins[part].y, v.z + origins[part].z};
                for (int k = 0; k < 3; ++k) {
                    if (!any || p[k] < lo[k]) lo[k] = p[k];
                    if (!any || p[k] > hi[k]) hi[k] = p[k];
                }
                any = true;
            }
    }
    if (!any) return INT32_MAX;
    int32_t worst = 0;
    for (int k = 0; k < 3; ++k) {
        const int32_t centre = (k == 0 ? group.bbox.centre.x : k == 1 ? group.bbox.centre.y : group.bbox.centre.z);
        const int32_t half = group.bbox.half[k];
        worst = std::max(worst, std::abs(lo[k] - (centre - half)));
        worst = std::max(worst, std::abs(hi[k] - (centre + half)));
    }
    return worst;
}

} // namespace

Assembly AssembleGroup(const ModelGroup& group, const SkeletonTable& skeleton) {
    Assembly best;
    best.origins.assign(group.subMeshes.size(), SVector{});
    if (group.subMeshes.size() <= 1 || group.verts.empty()) {
        best.assembled = true; // a single part is already standing up
        return best;
    }

    const std::vector<const AttachProgram*> candidates = skeleton.Candidates(group.subMeshes.size());
    int32_t bestError = INT32_MAX;
    std::vector<SVector> origins;
    for (const AttachProgram* program : candidates) {
        if (!OriginsFor(group, *program, origins)) continue;
        const int32_t error = BoxError(group, origins);
        if (error < bestError) {
            bestError = error;
            best.origins = origins;
            best.programIndex = static_cast<size_t>(program - skeleton.Programs().data());
            best.boxError = error;
            best.assembled = true;
        }
    }
    return best;
}

TriangleSoup BuildAssembledTriangleSoup(const ModelGroup& group, const Assembly& assembly) {
    TriangleSoup soup = BuildTriangleSoup(group);
    const float factor = static_cast<float>(LodFactor(group));

    size_t vertexIndex = 0;
    for (size_t part = 0; part < group.subMeshes.size(); ++part) {
        const SVector origin = part < assembly.origins.size() ? assembly.origins[part] : SVector{};
        const size_t count = group.subMeshes[part].prims.size() * 6;
        for (size_t i = 0; i < count && vertexIndex < soup.vertices.size(); ++i, ++vertexIndex) {
            TriangleSoup::Vertex& v = soup.vertices[vertexIndex];
            v.x = (v.x + static_cast<float>(origin.x)) * factor;
            v.y = (v.y + static_cast<float>(origin.y)) * factor;
            v.z = (v.z + static_cast<float>(origin.z)) * factor;
        }
    }
    return soup;
}

TriangleSoup BuildTriangleSoup(const ModelGroup& group) {
    TriangleSoup soup;
    const auto normalOf = [&](uint16_t vertexIndex) -> SVector {
        if (vertexIndex < group.vertNormalIndex.size()) {
            const uint16_t n = group.vertNormalIndex[vertexIndex];
            if (n < group.normals.size()) return group.normals[n];
        }
        return SVector{};
    };
    const auto emit = [&](const Primitive& prim, int corner) {
        const uint16_t vi = prim.index[static_cast<size_t>(corner)];
        const SVector& p = group.verts[vi];
        const SVector n = normalOf(vi);
        TriangleSoup::Vertex out;
        out.x = static_cast<float>(p.x);
        out.y = static_cast<float>(p.y);
        out.z = static_cast<float>(p.z);
        // PS1 normals are unit vectors in 1.0 = 4096 fixed point.
        out.nx = static_cast<float>(n.x) / 4096.0f;
        out.ny = static_cast<float>(n.y) / 4096.0f;
        out.nz = static_cast<float>(n.z) / 4096.0f;
        out.u = static_cast<float>(prim.u[static_cast<size_t>(corner)]);
        out.v = static_cast<float>(prim.v[static_cast<size_t>(corner)]);
        out.clut = prim.clut;
        out.tpage = prim.tpage;
        out.window[3] = (prim.clut & 0x8000u) ? 0u : 1u; // one-sided unless clut bit 15 (rmd3.h)
        soup.vertices.push_back(out);
    };
    for (const SubMesh& sub : group.subMeshes) {
        for (const Primitive& prim : sub.prims) {
            // The four corners are written along the PERIMETER - corner 2 sits opposite corner 0 -
            // and NOT in the PS1 `POLY_FT4` strip order, where corner 3 is the one opposite. This
            // is the same convention the scene cells turned out to use
            // (docs\formats\scene_cell.md 4.1.1), but it had to be measured here separately because
            // models are a different format: `rrtool quadwind` splits every quad of every model both
            // ways and asks which split gives two triangles wound the SAME way. Over all 112 .GEO
            // files, 35653 quads: perimeter order is consistent for 33074 of them, strip order for
            // 1152. Splitting the strip way folds a quad into a bowtie and loses the quarter between
            // its diagonals, which is what put a black wedge through the roadside signs.
            // Six soup vertices a primitive (rmd3.h SoupCorners): a quad's two triangles on the emitter's i1-i3
            // diagonal; a 3-corner primitive's one triangle (i0, i1, i2) and a zero-area (i2, i2, i2) - the emitter's
            // GT3 arm SLUS 0x80025BDC draws nothing of the record's fourth index.
            for (int k = 0; k < 6; ++k) emit(prim, SoupCorners(prim)[k]);
        }
    }
    return soup;
}

} // namespace rr
