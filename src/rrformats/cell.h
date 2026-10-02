#pragma once
// Scene cells - the world beside the road (types 0, 8 and 9 of the 0x4000 stream chunk).
//
// Type 0 is a complete cell, type 8 is a cell whose region 7 did not fit in 16 KiB, and type 9 is
// that missing region carried under the same resource id. Layout proven in docs\formats\scene_cell.md;
// this is the authoritative parser (tools\scout\cell.py stays as an independent cross-check).
#include "rrformats/chunk.h"

#include <cstdint>
#include <span>
#include <vector>

namespace rr {

// Both the cell origin and its vertices are stored in world units * 64 - the same factor the road
// geometry uses. `RASHCDG 0x800A8584` shifts the origin left by 10 into a 16.16 world vector.
constexpr float kCellUnitsPerWorldUnit = 64.0f;

// Two texture references that name no resource of the cell's own.
//   * `0x7800` is `30 << 10`. The fix-up pass special-cases it (`SLUS_010.53 0x800340F4` and
//     `0x80034294` compare `(texRef >> 10) & 0x1F` against 30) and takes its page from the single
//     runtime record at guest `0x800D6160` instead of from the cell's own texture pair. That page is
//     not shipped as a chunk - see docs\formats\scene_cell.md 12.5.
//   * `0x7C00` is `31 << 10` and is band 2's constant. No pass touches it.
constexpr uint16_t kCellTexRuntimePage = 0x7800;
constexpr uint16_t kCellTexBand2 = 0x7C00;

struct CellPrimitive {
    // Which record of the region-2 group table this primitive came from. The draw dispatcher
    // (`RASHCDG 0x80068FCC`) works one group at a time and chooses between the coarse group `A+k`
    // and the fine pair `A+B+k` / `A+2B+k`, so a renderer that wants to reproduce that choice has
    // to keep the group a primitive belongs to.
    uint16_t group = 0;
    uint8_t flags = 0;
    // The palette selector, straight into the 32 palette rows of the page this primitive samples
    // (docs\formats\scene_cell.md 12). `SLUS_010.53 0x80022758`/`0x8002289c` read it at record +0x01
    // and turn it into a CLUT row; on our bytes it never exceeds 29.
    uint8_t pal = 0;
    // The halfword at record +0x02. Its top nibble is the SURFACE the ground query hands the bike:
    // `RASHCDG 0x800A8360`/`0x800A8374` load it `lhu` and store `>> 12` to entity+0x216.
    // Census over the primitives the walk lists reference:
    // coarse 0..15, fine 0..8. The low twelve bits are not read by any code this project has read.
    uint16_t attr = 0;
    uint8_t u[4] = {};
    uint8_t v[4] = {};
    uint16_t clut = 0;   // placeholder on disc; the loader overwrites it
    uint16_t texRef = 0; // a packed id from the cell's own header pair, or 0x7C00 / 0x7800
    uint16_t index[4] = {};
    bool quad = false;
};

// One entry of the primitive group table (region 2). Groups fall into three bands; band 0 lives in
// region 6 and bands 1 and 2 in region 7.
//
// The draw dispatcher `RASHCDG 0x80068FCC` (overlay sha1 cfe43a7786759f2cb9c57751cf99e84d1074782c)
// reads the table as `A` groups that are always drawn coarse plus `B` groups that are drawn EITHER
// coarse OR fine:
//   * `0x8006901C`..`0x80069058`: for k in [0, A) draw region-6 group k (`0x8006D350`).
//   * `0x800690E8`..`0x800691E0`: for k in [0, B) take the nibble `(ctx[+0x34] >> (4*k)) & 0xF`.
//     Bit 0 clear skips the group entirely. Bit 1 set draws the FINE pair - region-7 group
//     `A+2B+k` (`0x8006E474` / `0x8006F5D0`, quads) and region-7 group `A+B+k` (`0x8006A630` /
//     `0x8006C888`, triangles) - and bit 1 clear draws the COARSE region-6 group `A+k`
//     (`0x8006D350` / `0x8006DC20`). Bit 2 picks between the two routines of each pair.
//   * when the global at `0x800CC86C` is non-zero the nibble is masked with `0x0D`, which clears
//     bit 1, i.e. forces every group coarse.
// So bands 1 and 2 are not a separate world: they are the fine version of band 0's last B groups.
struct CellPrimitiveGroup {
    uint32_t byteOffset = 0;
    uint32_t triCount = 0;
    uint32_t quadCount = 0;
    int band = 0;
};

// One placed object from region 0. Everything here is absolute - no reconstruction needed.
// `kind` is the entity pool the object belongs to (docs\formats\population.md): 4 is a roadside
// prop, whose `cls` is a group index into model id 200 (the HAZARD*.GEO prop sheet), 6 is a static
// collision volume, 2 a pedestrian; kinds 0 and 3 ship empty.
struct CellPlacement {
    uint16_t kind = 0;
    uint16_t cls = 0;
    uint32_t pieceKey = 0;
    int16_t orientation[3] = {}; // unit vector, 4096 = 1.0
    int32_t pos[3] = {};         // 16.16 world, Y down
    int32_t lateral = 0;         // signed offset from the centre line, 16.16
    int32_t along = 0;           // distance along the road, 16.16
};

// Region 4 of a cell: one record of the ground query's walk lists.
//   tag bit 7    fine (the primitive group A+B+sub of region 7) / coarse (group A+sub of region 6)
//   tag bits 4..6 the sub-area; bits 2..3 the crossing edge it is entered by;
//   tag bits 0..1 which list: coarse records come in pairs (lists 1, 3), fine in trios (1, 2, 3)
// Each entry is an index into the group's primitives, triangles first, then quads.
struct CellWalkRecord {
    uint16_t offset = 0;          // byte offset of the record inside region 4 (the query's cache field)
    uint8_t tag = 0;
    std::vector<uint8_t> entries; // `len` bytes
};

struct CellData {
    ChunkHeader header;
    uint16_t countA = 0;
    uint16_t countB = 0;
    // The cell's own header texture pair, split the way the loader files it
    // (`SLUS_010.53 0x8003237C`..`0x800323C4` writes the values with bit 15 to slot `+0x58/+0x5C`
    // and the ones without to `+0x50/+0x54`, skipping `0xFFFF`). Which half a primitive looks in is
    // decided by the band it belongs to, not by the primitive: the band-0 fix-up pass
    // (`0x80034428`) hands the resolver the `+0x58/+0x5C` keys and the band-1 pass (`0x80033f94`)
    // the `+0x50/+0x54` ones. A key names the texture resource of the same id as the cell:
    // `resourceId = ((key >> 10) & 0x1F) << 23 | (key & 0x3FF)`.
    std::vector<uint16_t> texKeyBand0; // bit 15 set   -> the type-2 chunk of that id
    std::vector<uint16_t> texKeyBand1; // bit 15 clear -> the two type-1 chunks of that id
    int32_t origin[3] = {};
    std::vector<int16_t> vertexX, vertexY, vertexZ; // parallel arrays, cell units
    // The fourth halfword of a vertex. Its LOW BYTE is the vertex's shade: an index into the level's
    // 256-entry colour table (DATA\GAMEBIN1.DAT, bundle section 7; rrformats/level_bundle.h) that
    // the fine draw routines Gouraud-shade band 1 and band 2 with - `RASHCDG 0x8006CBF0..0x8006CC0C`
    // (band 1) and `0x8006E7C0..0x8006E7E4` (band 2) load `lbu 6(vertex)`, scale it by 4 and read
    // the colour word at `0x800D4CA8 + 4 * index` (docs\formats\scene_cell.md 13).
    std::vector<int16_t> vertexW;
    size_t vertexCount = 0;
    std::vector<CellPrimitiveGroup> groups;
    std::vector<CellPrimitive> band0; // region 6
    std::vector<CellPrimitive> band1; // region 7, present only when region 7 is in this chunk
    std::vector<CellPrimitive> band2; // region 7, continuing after band 1
    std::vector<CellPlacement> placements; // region 0
    bool region7Present = false;

    // Region 3: the sub-area boundary polygons the ground query's lookup tests the point against
    // (`RASHCDG 0x800A8498`). The region is a flat array of
    // s16: `region3[0..B+1]` are offsets into this same array (`region3[0] == B + 2`), polygon `s`
    // runs from `region3[s+1]` to `region3[s+2]` and its halfwords are vertex indices (read `lh`,
    // signed). The run [region3[0], region3[1]) is never read by the lookup. The lookup stores each
    // polygon REVERSED, which decides which edge index its state word names.
    std::vector<int16_t> region3;
    // Region 4: `nCoarse` records with tag bit 7 clear come first, then the fine ones, `nAll` in
    // total.
    uint8_t walkCoarseCount = 0;
    uint8_t walkCount = 0;
    std::vector<CellWalkRecord> walk;
};

// The vertex indices of region-3 polygon `sub` (0 <= sub < B), in the order they are STORED on the
// disc (the lookup reverses them). Empty when the offsets do not describe a polygon.
std::vector<int16_t> CellSubAreaPolygon(const CellData& cell, size_t sub);

// Which band a region-2 group index belongs to, and which group of the neighbouring bands is its
// counterpart. Band 0 is `[0, A+B)`, band 1 `[A+B, A+2B)` and band 2 `[A+2B, A+3B)`; the coarse
// group `A+k` and the fine groups `A+B+k` and `A+2B+k` are the same piece of ground.
inline size_t CellFineGroupCount(const CellData& cell) { return cell.countB; }
inline size_t CellAlwaysGroupCount(const CellData& cell) { return cell.countA; }

// Parses a type-0 or type-8 scene cell. Throws if the chunk is not a scene cell or the region
// bookkeeping does not add up - the layout is exact, so a mismatch means we are wrong.
CellData ParseCellChunk(std::span<const uint8_t> chunk);

// Joins a type-9 chunk to the type-8 cell of the same resource id, which is the only way a type-8
// cell ever gets its bands 1 and 2: `SLUS_010.53 0x80032B7C` looks the cell up by id and
// `0x80032338` stores the type-9 chunk's payload (`chunk + 0x20`) into `body+0x3C`, i.e. as the
// cell's region 7 (docs\formats\scene_cell.md 3.2). The group byte offsets of bands 1 and 2 are
// measured from that payload, which is why nothing but the chunk itself is needed here.
// Throws if the chunk is not a type-9 chunk or its id does not match the cell's.
void AttachCellRegion7(CellData& cell, std::span<const uint8_t> type9Chunk);

// World position of a placed object, in the same world units the road geometry uses.
inline float PlacementWorldX(const CellPlacement& p) { return static_cast<float>(p.pos[0]) / 65536.0f; }
inline float PlacementWorldY(const CellPlacement& p) { return static_cast<float>(p.pos[1]) / 65536.0f; }
inline float PlacementWorldZ(const CellPlacement& p) { return static_cast<float>(p.pos[2]) / 65536.0f; }

// Which of a band's keys a primitive's `texRef` selects, reproducing the search both resolvers run
// (`SLUS_010.53 0x80022764`..`0x800227c0` and `0x800228a0`..`0x800228f4`) over the list the fix-up
// pass hands them: the keys sorted DESCENDING by `0x80033DAC`, not the disc order the cell stores
// them in. Walk that list while `texRef < key`, then fall back to its first (largest) key unless
// the key found is exactly `texRef`. `keys` is in disc order and the result indexes it. Returns -1
// when the list is empty, i.e. when the cell has no texture of that kind at all.
int CellTextureSlot(const std::vector<uint16_t>& keys, uint16_t texRef);

// World position of a cell vertex, in the same world units the road geometry uses.
inline float CellWorldX(const CellData& cell, size_t i) {
    return static_cast<float>(cell.origin[0] + cell.vertexX[i]) / kCellUnitsPerWorldUnit;
}
inline float CellWorldY(const CellData& cell, size_t i) {
    return static_cast<float>(cell.origin[1] + cell.vertexY[i]) / kCellUnitsPerWorldUnit;
}
inline float CellWorldZ(const CellData& cell, size_t i) {
    return static_cast<float>(cell.origin[2] + cell.vertexZ[i]) / kCellUnitsPerWorldUnit;
}

} // namespace rr
