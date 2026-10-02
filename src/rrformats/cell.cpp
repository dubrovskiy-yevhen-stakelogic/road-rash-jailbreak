#include "rrformats/cell.h"

#include <algorithm>

#include <stdexcept>
#include <string>

namespace rr {
namespace {

constexpr size_t kPayload = 0x20; // every chunk's payload starts here

uint16_t ReadU16(std::span<const uint8_t> d, size_t off) {
    if (off + 2 > d.size()) throw std::runtime_error("cell: read past end of chunk");
    return static_cast<uint16_t>(d[off] | (d[off + 1] << 8));
}

int16_t ReadS16(std::span<const uint8_t> d, size_t off) { return static_cast<int16_t>(ReadU16(d, off)); }

uint32_t ReadU32(std::span<const uint8_t> d, size_t off) {
    if (off + 4 > d.size()) throw std::runtime_error("cell: read past end of chunk");
    return static_cast<uint32_t>(d[off]) | (static_cast<uint32_t>(d[off + 1]) << 8) |
           (static_cast<uint32_t>(d[off + 2]) << 16) | (static_cast<uint32_t>(d[off + 3]) << 24);
}

int32_t ReadS32(std::span<const uint8_t> d, size_t off) { return static_cast<int32_t>(ReadU32(d, off)); }

void Expect(bool condition, const char* what) {
    if (!condition) throw std::runtime_error(std::string("cell: ") + what);
}

CellPrimitive ReadPrimitive(std::span<const uint8_t> d, size_t at, bool quad, size_t group) {
    CellPrimitive prim;
    prim.group = static_cast<uint16_t>(group);
    prim.quad = quad;
    prim.flags = d[at + 0x00];
    prim.pal = d[at + 0x01];
    prim.attr = ReadU16(d, at + 0x02);
    prim.u[0] = d[at + 0x04];
    prim.v[0] = d[at + 0x05];
    prim.clut = ReadU16(d, at + 0x06);
    prim.u[1] = d[at + 0x08];
    prim.v[1] = d[at + 0x09];
    prim.texRef = ReadU16(d, at + 0x0A);
    prim.u[2] = d[at + 0x0C];
    prim.v[2] = d[at + 0x0D];
    if (quad) {
        prim.u[3] = d[at + 0x0E];
        prim.v[3] = d[at + 0x0F];
        for (size_t k = 0; k < 4; ++k) prim.index[k] = ReadU16(d, at + 0x10 + k * 2);
    } else {
        for (size_t k = 0; k < 3; ++k) prim.index[k] = ReadU16(d, at + 0x0E + k * 2);
    }
    return prim;
}

// Reads the primitives of one band out of its list. Inside a group the triangles come first, then
// the quads - which is what makes `byteOffset` the running sum of tri*20 + quad*24. The two draw
// routines confirm the split: `RASHCDG 0x8006D350` reads `group+4` (the triangle count) and walks
// 20-byte records, `0x8006E474` reads `group+8` and walks 24-byte ones.
void ReadBand(std::span<const uint8_t> d, size_t listBase, const std::vector<CellPrimitiveGroup>& groups, int band,
              size_t vertexCount, std::vector<CellPrimitive>& out) {
    for (size_t g = 0; g < groups.size(); ++g) {
        const CellPrimitiveGroup& group = groups[g];
        if (group.band != band) continue;
        size_t at = listBase + group.byteOffset;
        for (uint32_t i = 0; i < group.triCount; ++i, at += 20) out.push_back(ReadPrimitive(d, at, false, g));
        for (uint32_t i = 0; i < group.quadCount; ++i, at += 24) out.push_back(ReadPrimitive(d, at, true, g));
    }
    for (const CellPrimitive& prim : out) {
        const size_t corners = prim.quad ? 4u : 3u;
        for (size_t k = 0; k < corners; ++k)
            Expect(prim.index[k] < vertexCount, "primitive vertex index out of range");
    }
}

} // namespace

CellData ParseCellChunk(std::span<const uint8_t> chunk) {
    CellData cell;
    cell.header = ParseChunkHeader(chunk);
    const uint8_t type = cell.header.type;
    Expect(type == 0 || type == 8, "not a scene cell chunk (types 0 and 8)");

    const uint32_t nWords = ReadU32(chunk, kPayload);
    Expect(nWords >= 13, "nWords is too small to hold the fixed tail");
    const size_t body = kPayload + static_cast<size_t>(nWords) * 4;
    Expect(body + 0x40 <= chunk.size(), "cell body does not fit in the chunk");

    // The texture pair area: `nWords - 13` words read as 2*(nWords-13) halfwords, the loader's own
    // step count (`0x80032364`..`0x8003236C`). Each value is a packed texture key; `0xFFFF` is a pad.
    for (uint32_t k = 0; k < (nWords - 13) * 2; ++k) {
        const uint16_t value = ReadU16(chunk, kPayload + 4 + k * 2);
        if (value == 0xFFFF) continue;
        // At most two of each kind: the slot has room for two (`0x8003234C` counts each kind to 2).
        std::vector<uint16_t>& keys = (value & 0x8000) ? cell.texKeyBand0 : cell.texKeyBand1;
        if (keys.size() < 2) keys.push_back(value);
    }

    cell.countA = ReadU16(chunk, body + 0x04);
    cell.countB = ReadU16(chunk, body + 0x06);
    for (size_t k = 0; k < 3; ++k) cell.origin[k] = ReadS32(chunk, body + 0x08 + k * 4);

    // The eight region offsets are relative to the payload; the loader rewrites them in place as
    // absolute pointers, which is why a cell in a RAM dump does not match the disc bytes here.
    uint32_t region[8];
    for (size_t k = 0; k < 8; ++k) region[k] = ReadU32(chunk, body + 0x20 + k * 4);
    Expect(region[0] == (body - kPayload) + 0x40, "region 0 does not start at the end of the body header");
    for (size_t k = 1; k < 8; ++k)
        if (region[k] != 0) Expect(region[k] >= region[k - 1], "region offsets do not ascend");

    // Region 5: a four-byte prefix the consumer skips, then 8 bytes per vertex.
    const size_t vertexBase = kPayload + region[5] + 4;
    Expect(region[6] > region[5] + 4, "region 5 is empty");
    const size_t vertexBytes = region[6] - region[5] - 4;
    const size_t vertexCount = vertexBytes / 8;
    cell.vertexX.reserve(vertexCount);
    cell.vertexY.reserve(vertexCount);
    cell.vertexZ.reserve(vertexCount);
    cell.vertexW.reserve(vertexCount);
    for (size_t i = 0; i < vertexCount; ++i) {
        cell.vertexX.push_back(ReadS16(chunk, vertexBase + i * 8 + 0));
        cell.vertexY.push_back(ReadS16(chunk, vertexBase + i * 8 + 2));
        cell.vertexZ.push_back(ReadS16(chunk, vertexBase + i * 8 + 4));
        cell.vertexW.push_back(ReadS16(chunk, vertexBase + i * 8 + 6));
    }

    // Region 2: A + 3B group records of 12 bytes, in three bands.
    const size_t groupCount = static_cast<size_t>(cell.countA) + 3u * cell.countB;
    const size_t band0End = static_cast<size_t>(cell.countA) + cell.countB;
    const size_t band1End = static_cast<size_t>(cell.countA) + 2u * cell.countB;
    cell.groups.reserve(groupCount);
    for (size_t i = 0; i < groupCount; ++i) {
        const size_t at = kPayload + region[2] + i * 12;
        CellPrimitiveGroup group;
        group.byteOffset = ReadU32(chunk, at + 0);
        group.triCount = ReadU32(chunk, at + 4);
        group.quadCount = ReadU32(chunk, at + 8);
        group.band = i < band0End ? 0 : (i < band1End ? 1 : 2);
        cell.groups.push_back(group);
    }

    // Region 0: five arrays of placed objects, one per entity pool. The header gives a count and a
    // relative offset per array, and the strides come from the classify helper RASHCDG 0x8009C41C.
    {
        const size_t base = kPayload + region[0];
        constexpr uint16_t kKind[5] = {3, 2, 4, 6, 0};
        constexpr size_t kStride[5] = {68, 76, 64, 88, 64};
        for (size_t a = 0; a < 5; ++a) {
            const uint16_t count = ReadU16(chunk, base + a * 2);
            const uint32_t arrayOffset = ReadU32(chunk, base + 0x24 + a * 4);
            for (uint16_t i = 0; i < count; ++i) {
                const size_t at = base + arrayOffset + static_cast<size_t>(i) * kStride[a];
                if (at + kStride[a] > chunk.size()) throw std::runtime_error("cell: placement runs past the chunk");
                CellPlacement placement;
                placement.kind = ReadU16(chunk, at + 0x00);
                placement.cls = ReadU16(chunk, at + 0x02);
                placement.pieceKey = ReadU32(chunk, at + 0x08);
                for (size_t k = 0; k < 3; ++k) placement.orientation[k] = ReadS16(chunk, at + 0x0E + k * 2);
                for (size_t k = 0; k < 3; ++k) placement.pos[k] = ReadS32(chunk, at + 0x14 + k * 4);
                placement.lateral = ReadS32(chunk, at + 0x20);
                placement.along = ReadS32(chunk, at + 0x24);
                Expect(placement.kind == kKind[a], "placement kind does not match the array it is in");
                cell.placements.push_back(placement);
            }
        }
    }

    // Region 3: the sub-area polygons, one flat s16 array. Its size is exact:
    // up to region 4. The offsets are indices into the array itself.
    {
        Expect(region[4] >= region[3] && (region[4] - region[3]) % 2 == 0, "region 3 is not a halfword array");
        const size_t count = (region[4] - region[3]) / 2;
        const size_t B = cell.countB;
        Expect(count >= B + 2, "region 3 is shorter than its own offset table");
        cell.region3.reserve(count);
        for (size_t k = 0; k < count; ++k) cell.region3.push_back(ReadS16(chunk, kPayload + region[3] + 2 * k));
        Expect(cell.region3[0] == static_cast<int16_t>(B + 2), "region 3 does not start with B + 2");
        for (size_t k = 0; k + 1 < B + 2; ++k)
            Expect(cell.region3[k] < cell.region3[k + 1], "region 3 offsets do not ascend");
        Expect(static_cast<size_t>(cell.region3[B + 1]) <= count, "region 3 polygon runs past the region");
        for (size_t k = static_cast<size_t>(cell.region3[1]); k < static_cast<size_t>(cell.region3[B + 1]); ++k)
            Expect(cell.region3[k] >= 0 && static_cast<size_t>(cell.region3[k]) < vertexCount,
                   "region 3 vertex index out of range");
    }
    // Region 4: the walk lists. Every record starts below byte 512, the ten bits the query caches.
    {
        const size_t base = kPayload + region[4];
        const size_t end = kPayload + region[5];
        Expect(base + 2 <= end, "region 4 is too short for its two counts");
        cell.walkCoarseCount = chunk[base];
        cell.walkCount = chunk[base + 1];
        Expect(cell.walkCoarseCount <= cell.walkCount, "region 4 has more coarse records than records");
        size_t off = 2;
        for (size_t k = 0; k < cell.walkCount; ++k) {
            Expect(base + off + 2 <= end, "region 4 record header runs past the region");
            Expect(off < 512, "region 4 record starts past byte 511");
            CellWalkRecord rec;
            rec.offset = static_cast<uint16_t>(off);
            rec.tag = chunk[base + off];
            const size_t len = chunk[base + off + 1];
            Expect(base + off + 2 + len <= end, "region 4 record runs past the region");
            Expect(((rec.tag >> 7) != 0) == (k >= cell.walkCoarseCount),
                   "region 4 coarse records are not the first nCoarse");
            rec.entries.assign(chunk.begin() + static_cast<std::ptrdiff_t>(base + off + 2),
                               chunk.begin() + static_cast<std::ptrdiff_t>(base + off + 2 + len));
            cell.walk.push_back(std::move(rec));
            off += 2 + len;
        }
    }

    ReadBand(chunk, kPayload + region[6], cell.groups, 0, vertexCount, cell.band0);
    // A type-8 cell's region 7 lives in the matching type-9 chunk, so it is simply absent here;
    // `AttachCellRegion7` is what fills bands 1 and 2 in that case.
    cell.region7Present = type == 0 && region[7] != 0;
    if (cell.region7Present) {
        // Bands 1 and 2 share ONE offset space that starts at region 7, which is why both are read
        // with the same base. `RASHCDG 0x800706A4` adds a band-2 group's `byteOffset` straight to
        // `body+0x3C`, and the type-9 reconstruction of scene_cell.md 3.2 walks groups A+B..A+3B
        // from offset 0 of the type-9 payload with 0 mismatches over 540 cells.
        ReadBand(chunk, kPayload + region[7], cell.groups, 1, vertexCount, cell.band1);
        ReadBand(chunk, kPayload + region[7], cell.groups, 2, vertexCount, cell.band2);
    }
    cell.vertexCount = vertexCount;
    return cell;
}

void AttachCellRegion7(CellData& cell, std::span<const uint8_t> type9Chunk) {
    const ChunkHeader header = ParseChunkHeader(type9Chunk);
    Expect(header.type == 9, "not a type-9 chunk");
    Expect(header.id == cell.header.id, "type-9 chunk carries another resource id");
    Expect(!cell.region7Present, "this cell already has its region 7");
    cell.band1.clear();
    cell.band2.clear();
    // `0x80032338` stores `chunk + 0x20` into `body+0x3C`, so the payload start IS region 7 and the
    // group byte offsets are measured from it.
    ReadBand(type9Chunk, kPayload, cell.groups, 1, cell.vertexCount, cell.band1);
    ReadBand(type9Chunk, kPayload, cell.groups, 2, cell.vertexCount, cell.band2);
    cell.region7Present = true;
}

std::vector<int16_t> CellSubAreaPolygon(const CellData& cell, size_t sub) {
    std::vector<int16_t> out;
    if (sub >= cell.countB || cell.region3.size() < static_cast<size_t>(cell.countB) + 2u) return out;
    const int32_t from = cell.region3[sub + 1], to = cell.region3[sub + 2];
    if (from < 0 || to < from || static_cast<size_t>(to) > cell.region3.size()) return out;
    out.assign(cell.region3.begin() + from, cell.region3.begin() + to);
    return out;
}

int CellTextureSlot(const std::vector<uint16_t>& keys, uint16_t texRef) {
    if (keys.empty()) return -1;
    // The loader files the keys in DISC order (`0x8003234C`, slot +0x50/+0x54 and +0x58/+0x5C), but
    // neither fix-up pass hands that order to the resolver: `0x80033F14` copies the two keys and
    // their records into a local array, stores a -1 sentinel after them and calls `0x80033DAC`,
    // which sorts the keys DESCENDING and swaps the records with them (`0x80033D70`). The resolver
    // then walks that sorted list while `texRef < key` (`0x80022774`/`0x80022794`, `slt texRef,key`)
    // and falls back to entry 0 of the SORTED list - the larger key - when the key it stops on is
    // not `texRef`. Searching the disc order instead sends every primitive of an ascending pair's
    // second key to the first key's page (scene_cell.md 12.2).
    std::vector<size_t> order(keys.size());
    for (size_t k = 0; k < order.size(); ++k) order[k] = k;
    // `0x80033DAC` is a three-element exchange network over signed words; the keys are zero-extended
    // halfwords, so it orders them as unsigned 16-bit values, and equal keys are never exchanged.
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return keys[a] > keys[b]; });
    size_t at = 0;
    while (at < order.size() && texRef < keys[order[at]]) ++at;
    if (at == order.size() || keys[order[at]] != texRef) at = 0;
    return static_cast<int>(order[at]);
}

} // namespace rr
