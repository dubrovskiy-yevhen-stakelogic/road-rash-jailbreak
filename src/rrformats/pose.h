#pragma once
// Part poses for RMD3 models: the live 3x3 rotations the original keeps per sub-mesh, what they
// are made of, and how they compose down the attachment chain.
//
// Header-only on purpose: it adds no translation unit, so the build file does not have to change.
//
// WHAT IS ESTABLISHED (docs\formats\rmd3.md section 11):
//
//  * The runtime per-part slot is 24 bytes - `u32 dpd3` then a 3x3 of `s16` at +0x04 - allocated by
//    `SLUS_010.53 0x8002FDEC` and initialised to the identity (4096 = 1.0).  Slot `i` belongs to
//    sub-mesh `i`: the assembly loop at `RASHCDG 0x800672E8` steps the pointer by 24 per link word
//    and uses the loop index as the child part.
//  * The stored 3x3 is a LOCAL rotation, relative to the parent part.  `RASHCDG 0x800671DC`
//    loads the parent's ACCUMULATED matrix into the GTE rotation registers and multiplies the
//    child's slot matrix through it one column at a time (`lhu` at +0, +6, +12 of the slot, then
//    +2 and +4), storing the product back on a 32-byte scratchpad stack.  So
//        world(child) = world(parent) * local(child)
//    with both matrices row-major in the GTE's own layout (R11 R12 R13 R21 ... R33).
//  * Slot 0 of an object carries the object's own world orientation: the placement path at
//    `RASHCDG 0x80066DC8` reads `parts + 4` and feeds it to the GTE as the object's matrix.
//  * Every one of those matrices is `RotMatrix(vx, vy, vz)` of an integer Euler triple in the
//    PS1 unit of 4096 per full turn, in the psyq order M = Rz(vz) * Ry(vy) * Rx(vx), built from
//    the 4096-entry (sin, cos) table at guest `0x8005624C` (`SLUS_010.53 +0x4644C`).  Measured:
//    all 22 live matrices of the player's bike and rider in `work\oracle\state\rr-race\ram.bin`
//    are reproduced by that construction to within 3 units of 4096 - see `rrview --posecheck`.
//
// WHAT IS NOT: where the angles come from frame to frame.  The `DMD3` payload of `ANIMTBL*.PSX`
// (rmd3.md section 5) is still undecoded, so a pose has to be MEASURED out of a
// capture rather than computed from the disc.  Nothing in this repository stores one.
#include "rrformats/rmd3.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

namespace rr {

// A 3x3 in the GTE's own layout: row-major, 4096 = 1.0.
struct PartMatrix {
    int16_t m[9] = {4096, 0, 0, 0, 4096, 0, 0, 0, 4096};
    int16_t At(int row, int col) const { return m[row * 3 + col]; }
};

struct PartSlot {
    uint32_t dpd3 = 0;   // the sub-mesh's DPD3 pointer, as the binder wrote it
    PartMatrix rot;      // the LOCAL rotation of this part
};

// ------------------------------------------------------------------ the game's own sine table

// 4096 entries of (s16 sin, s16 cos), 4096 = 1.0, indexed by `angle & 0xFFF`.
// `RASHCDG 0x80063CE8` and `0x80063E20` both index it as `(angle & 0xFFF) * 4`.
struct SineTable {
    static constexpr uint32_t kAddress = 0x8005624Cu;
    static constexpr int kEntries = 4096;
    static constexpr uint32_t kExeLoad = 0x80010000u;
    static constexpr size_t kExeHeader = 0x800;

    std::array<int16_t, kEntries> sin{};
    std::array<int16_t, kEntries> cos{};

    // Reads it out of the player's own `SLUS_010.53`. The load address comes from the PS-X EXE
    // header rather than a constant, so a different build would still resolve.
    static SineTable FromExe(std::span<const uint8_t> exe) {
        if (exe.size() < kExeHeader || std::memcmp(exe.data(), "PS-X EXE", 8) != 0)
            throw std::runtime_error("pose: not a PS-X EXE");
        uint32_t load = 0;
        for (int i = 0; i < 4; ++i) load |= static_cast<uint32_t>(exe[0x18 + i]) << (8 * i);
        if (kAddress < load) throw std::runtime_error("pose: sine table is below the EXE load address");
        const size_t at = kExeHeader + static_cast<size_t>(kAddress - load);
        if (at + static_cast<size_t>(kEntries) * 4 > exe.size())
            throw std::runtime_error("pose: EXE too short for the sine table");
        SineTable table;
        for (int i = 0; i < kEntries; ++i) {
            const size_t record = at + static_cast<size_t>(i) * 4;
            table.sin[static_cast<size_t>(i)] =
                static_cast<int16_t>(exe[record] | (exe[record + 1] << 8));
            table.cos[static_cast<size_t>(i)] =
                static_cast<int16_t>(exe[record + 2] | (exe[record + 3] << 8));
        }
        // The table is its own check: entry 0 must be (0, 4096) and entry 1024 (4096, 0).
        if (table.sin[0] != 0 || table.cos[0] != 4096 || table.sin[1024] != 4096 || table.cos[1024] != 0)
            throw std::runtime_error("pose: the sine table does not read as (sin, cos)");
        return table;
    }
};

// ------------------------------------------------------------------ RotMatrix and its inverse

inline PartMatrix Multiply3x3(const PartMatrix& a, const PartMatrix& b) {
    PartMatrix out;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            int32_t sum = 0;
            for (int k = 0; k < 3; ++k)
                sum += static_cast<int32_t>(a.m[r * 3 + k]) * static_cast<int32_t>(b.m[k * 3 + c]);
            out.m[r * 3 + c] = static_cast<int16_t>(sum >> 12);
        }
    return out;
}

inline PartMatrix Transpose3x3(const PartMatrix& a) {
    PartMatrix out;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) out.m[r * 3 + c] = a.m[c * 3 + r];
    return out;
}

// M = Rz(az) * Ry(ay) * Rx(ax), the psyq `RotMatrix` order, in the game's own fixed point.
inline PartMatrix RotMatrixZYX(const SineTable& table, int ax, int ay, int az) {
    const auto entry = [&](int a, int16_t& s, int16_t& c) {
        const size_t i = static_cast<size_t>(a & 0xFFF);
        s = table.sin[i];
        c = table.cos[i];
    };
    int16_t sx, cx, sy, cy, sz, cz;
    entry(ax, sx, cx);
    entry(ay, sy, cy);
    entry(az, sz, cz);
    PartMatrix rx, ry, rz;
    rx.m[0] = 4096; rx.m[1] = 0;   rx.m[2] = 0;
    rx.m[3] = 0;    rx.m[4] = cx;  rx.m[5] = static_cast<int16_t>(-sx);
    rx.m[6] = 0;    rx.m[7] = sx;  rx.m[8] = cx;
    ry.m[0] = cy;   ry.m[1] = 0;   ry.m[2] = sy;
    ry.m[3] = 0;    ry.m[4] = 4096; ry.m[5] = 0;
    ry.m[6] = static_cast<int16_t>(-sy); ry.m[7] = 0; ry.m[8] = cy;
    rz.m[0] = cz;   rz.m[1] = static_cast<int16_t>(-sz); rz.m[2] = 0;
    rz.m[3] = sz;   rz.m[4] = cz;  rz.m[5] = 0;
    rz.m[6] = 0;    rz.m[7] = 0;   rz.m[8] = 4096;
    return Multiply3x3(Multiply3x3(rz, ry), rx);
}

// The closed-form inverse of the above, refined over a +-3 neighbourhood so the answer is the
// integer triple that reproduces the matrix best rather than the one a float rounds to.
// Returns the largest element difference between `RotMatrixZYX(out)` and `in`.
inline int DecomposeZYX(const SineTable& table, const PartMatrix& in, int out[3]) {
    const auto element = [&](int i) { return static_cast<double>(in.m[i]) / 4096.0; };
    const double sy = std::fmax(-1.0, std::fmin(1.0, -element(6)));
    const double y = std::asin(sy);
    double x, z;
    if (std::fabs(std::cos(y)) > 1e-6) {
        x = std::atan2(element(7), element(8));
        z = std::atan2(element(3), element(0));
    } else {
        x = 0.0;
        z = std::atan2(-element(1), element(4));
    }
    const double kTurn = 4096.0 / (2.0 * 3.14159265358979323846);
    const int seed[3] = {static_cast<int>(std::lround(x * kTurn)) & 0xFFF,
                         static_cast<int>(std::lround(y * kTurn)) & 0xFFF,
                         static_cast<int>(std::lround(z * kTurn)) & 0xFFF};
    int best = -1;
    for (int dx = -3; dx <= 3; ++dx)
        for (int dy = -3; dy <= 3; ++dy)
            for (int dz = -3; dz <= 3; ++dz) {
                const int trial[3] = {(seed[0] + dx) & 0xFFF, (seed[1] + dy) & 0xFFF,
                                      (seed[2] + dz) & 0xFFF};
                const PartMatrix built = RotMatrixZYX(table, trial[0], trial[1], trial[2]);
                int error = 0;
                for (int i = 0; i < 9; ++i)
                    error = std::max(error, std::abs(static_cast<int>(built.m[i]) - static_cast<int>(in.m[i])));
                if (best < 0 || error < best) {
                    best = error;
                    out[0] = trial[0];
                    out[1] = trial[1];
                    out[2] = trial[2];
                }
            }
    return best;
}

// ------------------------------------------------------------------ reading a captured pose

// The 24-byte runtime part slots of one object, out of a 2 MB guest RAM image. Nothing in this
// repository stores such an image or its contents; the caller passes its own dump.
inline std::vector<PartSlot> ReadPartSlots(std::span<const uint8_t> ram, uint32_t guestAddress,
                                           size_t count) {
    constexpr uint32_t kRamBase = 0x80000000u;
    if (guestAddress < kRamBase) throw std::runtime_error("pose: address is not in the KSEG0 RAM window");
    const size_t at = static_cast<size_t>(guestAddress - kRamBase);
    if (at + count * 24 > ram.size()) throw std::runtime_error("pose: RAM image too short for the part slots");
    std::vector<PartSlot> slots(count);
    for (size_t i = 0; i < count; ++i) {
        const size_t record = at + i * 24;
        uint32_t pointer = 0;
        for (int k = 0; k < 4; ++k) pointer |= static_cast<uint32_t>(ram[record + static_cast<size_t>(k)]) << (8 * k);
        slots[i].dpd3 = pointer;
        for (int k = 0; k < 9; ++k)
            slots[i].rot.m[k] = static_cast<int16_t>(ram[record + 4 + static_cast<size_t>(k) * 2] |
                                                     (ram[record + 5 + static_cast<size_t>(k) * 2] << 8));
    }
    return slots;
}

// A pose is only usable if the slots really are rotations. Row lengths within `slack` of 4096.
inline bool IsRotation(const PartMatrix& a, int slack = 8) {
    for (int r = 0; r < 3; ++r) {
        int32_t sum = 0;
        for (int c = 0; c < 3; ++c) sum += static_cast<int32_t>(a.m[r * 3 + c]) * a.m[r * 3 + c];
        const int length = static_cast<int>(std::lround(std::sqrt(static_cast<double>(sum))));
        if (std::abs(length - 4096) > slack) return false;
    }
    return true;
}

// ------------------------------------------------------------------ composing a pose

// World rotations and origins of every part, walking the attachment program the way
// `RASHCDG 0x80067064` walks it: origin(c) = origin(p) + world(p) * verts[vertBase(p) + k] and
// world(c) = world(p) * local(c).  `local` must hold one matrix per sub-mesh; an empty span is the
// rest pose (every part the identity), which reproduces `BuildAssembledTriangleSoup` exactly.
struct PosedGroup {
    std::vector<PartMatrix> world;
    std::vector<std::array<int32_t, 3>> origin;
};

inline PosedGroup PoseGroup(const ModelGroup& group, const SkeletonTable& skeleton,
                            const Assembly& assembly, std::span<const PartMatrix> local) {
    const size_t parts = group.subMeshes.size();
    PosedGroup posed;
    posed.world.assign(parts, PartMatrix{});
    posed.origin.assign(parts, {0, 0, 0});
    if (parts == 0) return posed;
    if (!local.empty() && local.size() != parts)
        throw std::runtime_error("pose: one local matrix per sub-mesh is required");
    const auto rotation = [&](size_t slot) {
        return (local.empty() || slot >= local.size()) ? PartMatrix{} : local[slot];
    };
    posed.world[0] = rotation(0);
    const AttachProgram* program = nullptr;
    if (assembly.assembled && parts > 1 && assembly.programIndex < skeleton.Programs().size())
        program = &skeleton.Programs()[assembly.programIndex];
    const int factor = LodFactor(group);
    for (size_t child = 1; child < parts; ++child) {
        size_t parent = 0;
        size_t slot = child;
        std::array<int32_t, 3> offset{0, 0, 0};
        if (program != nullptr && child - 1 < program->links.size()) {
            const AttachLink& link = program->links[child - 1];
            parent = link.parent;
            if (parent >= parts) parent = 0;
            // The matrix a link loads is named by its own field, not by the child index.
            slot = link.matrixPart < parts ? link.matrixPart : child;
            const size_t index = group.subMeshes[parent].vertBase + link.vertexOffset;
            if (index < group.verts.size()) {
                const SVector& vertex = group.verts[index];
                offset = {vertex.x * factor, vertex.y * factor, vertex.z * factor};
            }
        }
        const PartMatrix& parentWorld = posed.world[parent];
        std::array<int32_t, 3> turned{0, 0, 0};
        for (int r = 0; r < 3; ++r) {
            int64_t sum = 0;
            for (int c = 0; c < 3; ++c)
                sum += static_cast<int64_t>(parentWorld.m[r * 3 + c]) * offset[static_cast<size_t>(c)];
            turned[static_cast<size_t>(r)] = static_cast<int32_t>(sum >> 12);
        }
        for (int k = 0; k < 3; ++k)
            posed.origin[child][static_cast<size_t>(k)] =
                posed.origin[parent][static_cast<size_t>(k)] + turned[static_cast<size_t>(k)];
        posed.world[child] = Multiply3x3(parentWorld, rotation(slot));
    }
    return posed;
}

// The same triangles `BuildAssembledTriangleSoup` makes, with each part turned by its world
// rotation and moved to its posed origin. Vertex order, UVs and the per-primitive fields are
// untouched, so every texture check keeps working on the result.
inline TriangleSoup BuildPosedTriangleSoup(const ModelGroup& group, const PosedGroup& posed) {
    const int factor = LodFactor(group);
    // Which part owns each vertex, so a primitive that reaches outside its own sub-mesh still
    // moves with the part that authored the vertex - the same rule the rest-pose builder uses.
    std::vector<size_t> owner(group.verts.size(), 0);
    for (size_t part = 0; part < group.subMeshes.size(); ++part) {
        const SubMesh& sub = group.subMeshes[part];
        for (uint32_t i = 0; i < sub.vertCount; ++i) {
            const size_t index = sub.vertBase + i;
            if (index < owner.size()) owner[index] = part;
        }
    }
    const auto place = [&](uint16_t index, float out[3]) {
        const SVector& vertex = group.verts[index];
        const size_t part = owner[index];
        const int32_t local[3] = {static_cast<int32_t>(vertex.x) * factor,
                                  static_cast<int32_t>(vertex.y) * factor,
                                  static_cast<int32_t>(vertex.z) * factor};
        for (int r = 0; r < 3; ++r) {
            int64_t sum = 0;
            for (int c = 0; c < 3; ++c)
                sum += static_cast<int64_t>(posed.world[part].m[r * 3 + c]) * local[c];
            out[r] = static_cast<float>(static_cast<int32_t>(sum >> 12) +
                                        posed.origin[part][static_cast<size_t>(r)]);
        }
    };
    // The normal turns with the part too - the original feeds it through the same GTE matrix.
    const auto turn = [&](size_t part, const float in[3], float out[3]) {
        for (int r = 0; r < 3; ++r) {
            double sum = 0.0;
            for (int c = 0; c < 3; ++c)
                sum += static_cast<double>(posed.world[part].m[r * 3 + c]) * in[c];
            out[r] = static_cast<float>(sum / 4096.0);
        }
    };
    TriangleSoup soup;
    const TriangleSoup flat = BuildTriangleSoup(group);
    soup.vertices.reserve(flat.vertices.size());
    size_t cursor = 0;
    for (const SubMesh& sub : group.subMeshes) {
        for (const Primitive& prim : sub.prims) {
            // The same corners BuildTriangleSoup took for `flat` (rmd3.h SoupCorners): a quad's i1-i3 diagonal, a
            // 3-corner primitive's one triangle. (This read {0,1,2, 0,2,3} for every primitive, which since the quad
            // diagonal moved put a quad's positions and its texels on different corners.)
            const int* order = SoupCorners(prim);
            for (int k = 0; k < 6; ++k) {
                const int corner = order[k];
                if (cursor >= flat.vertices.size()) break;
                TriangleSoup::Vertex vertex = flat.vertices[cursor++];
                const uint16_t index = prim.index[static_cast<size_t>(corner)];
                float position[3];
                place(index, position);
                vertex.x = position[0];
                vertex.y = position[1];
                vertex.z = position[2];
                const float normal[3] = {vertex.nx, vertex.ny, vertex.nz};
                float turned[3];
                turn(owner[index], normal, turned);
                vertex.nx = turned[0];
                vertex.ny = turned[1];
                vertex.nz = turned[2];
                soup.vertices.push_back(vertex);
            }
        }
    }
    return soup;
}

} // namespace rr
