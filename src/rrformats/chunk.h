#pragma once
// The 0x4000 stream chunk of Road Rash: Jailbreak.
//
// A chunk is not a terrain tile - it is ONE RESOURCE, and the top nibble of its first word says
// which kind. Type 3 is the road geometry. Layout proven in docs\formats\road_chunk.md; this is the
// authoritative parser (the Python probe tools\scout\chunk.py stays as an independent cross-check).
#include <cstdint>
#include <span>
#include <vector>

namespace rr {

constexpr size_t kChunkSize = 0x4000;

enum class ChunkType : uint8_t {
    SceneCellA = 0,
    TextureA = 1,
    TextureB = 2,
    Road = 3,
    Panorama = 4,
    SpuAdpcm = 10,
};

// Where in the world this chunk has to be resident, in world units along a road.
struct ResidencyWindow {
    uint16_t road = 0;
    uint16_t from = 0;
    uint16_t to = 0;
};

struct ChunkHeader {
    uint32_t key = 0;
    uint8_t type = 0;      // key >> 28
    uint32_t id = 0;       // key & 0x0FFFFFFF
    uint16_t group = 0;    // id >> 16
    uint16_t index = 0;    // id & 0xFFFF
    std::vector<ResidencyWindow> windows;
};

ChunkHeader ParseChunkHeader(std::span<const uint8_t> chunk);

// The packed texture key a type-1 or type-2 chunk registers itself under. Both dispatcher arms build
// it the same way - `SLUS_010.53 0x80032cd0`..`0x80032d04` (type 1) and `0x80032c74`..`0x80032c8c`
// (type 2) - as `((id >> 23) & 0x1F) << 10 | (id & 0x3FF)`, with type 2 additionally setting bit 15.
// This is exactly the value a scene cell's header pair carries, which is what ties a cell to its
// textures. Meaningless for any other chunk type.
inline uint16_t PackedTextureKey(const ChunkHeader& header) {
    const uint16_t packed =
        static_cast<uint16_t>((((header.id >> 23) & 0x1Fu) << 10) | (header.id & 0x3FFu));
    return header.type == static_cast<uint8_t>(ChunkType::TextureB) ? static_cast<uint16_t>(packed | 0x8000u)
                                                                    : packed;
}

// One slice across the road: a full frame plus how far along the road it sits.
// `m` is a GTE matrix with 4096 = 1.0: row 2 is the unit tangent, row 0 the lateral axis, row 1 the
// surface normal (so its tilt off world Y is the banking). `pos` is 16.16 world, Y down.
struct RoadSlice {
    uint16_t index = 0;
    int16_t m[9] = {};
    int32_t pos[3] = {};
    uint32_t chord = 0;    // straight-line distance to the next slice
    uint32_t distance = 0; // running distance along the road
};

// A junction object holds several disjoint arms in one slice array; a run is one arm. The break is
// visible in the data (distance + chord == next.distance only inside a run), so no heuristic.
struct SliceRun {
    size_t first = 0;
    size_t count = 0;
};

struct RoadObject {
    ChunkHeader header;
    // The four words at chunk+0x20. `half` is 0xFFFFFFFF for a plain road piece; a junction ships
    // TWO objects that share a `pieceKey` and carry half 0 and half 1, and for those `owner` is the
    // NODE id. Measured over all 113 type-3 objects of set 1: 63 plain pieces, and 25 junctions with
    // exactly one half-0 and one half-1 object each.
    //
    // This matters because the half-1 object's GRPT names a ROAD, not the node - so filing junction
    // objects by their GRPT owner files that half under a road id and loses it. It holds three of
    // the junction's nine arms, including the two stubs that reach the roads the route actually
    // joins, which is why a junction could not be stitched from the core alone.
    uint32_t pieceKey = 0;
    uint32_t headerOwner = 0; // road id for a plain piece, node id for a junction half
    uint32_t half = 0xFFFFFFFFu;
    // From GRPT: where this object sits on its road. The objects of one road chain exactly -
    // `end` of one equals `start` of the next - which is what makes a route assemblable.
    uint32_t grptFlags = 0;   // 0x00010000 marks an intersection core rather than a road piece
    uint32_t ownerRoad = 0;   // road id, or node id when this is an intersection core
    uint32_t startAlongRoad = 0;
    uint32_t endAlongRoad = 0;
    bool IsIntersectionCore() const { return (grptFlags & 0x00010000u) != 0; }
    // True for either half of a junction. Both halves hold arms; neither is plain road.
    bool IsJunctionPart() const { return half != 0xFFFFFFFFu; }
    std::vector<RoadSlice> slices;
    std::vector<SliceRun> runs;
    float laneWidth = 0.0f; // world units, from XSAI
    int laneCount = 0;
};

// Parses a type-3 chunk. Throws if the chunk is not a road chunk or the sub-block chain is broken.
RoadObject ParseRoadChunk(std::span<const uint8_t> chunk);

// ------------------------------------------------------------------ type 4: the panorama
//
// The backdrop the game draws behind the world - the distant skyline, hills and treeline. The
// layout: `chunk + 0x20` holds
// `u32 3; u32 road; u32 distanceAlongRoad;` and then a tagged chain (the tag is a big-endian ASCII
// u32, so the bytes read backwards) of `PANO`, `STEN`, `OFFS`, `HORZ` and 55 `MDEC` sub-blocks:
//
//   * the panorama is **110 columns of 16 texels by 8 bands of 16 rows** = 1760 x 128;
//   * `STEN` is 880 two-bit codes, four per byte MSB first, one per (column, band): 0 means the
//     band is empty, 1 and 2 mean it carries a tile (2 additionally consumes a `HORZ` record);
//   * `MDEC` block `k` carries the tiles of columns `2k` and `2k+1` - the non-empty bands of the
//     even column top to bottom, then those of the odd one - as one 16-pixel-wide PS1 BS frame.
//
// `ParsePanoramaChunk` decodes the bitstreams and returns the assembled RGBA image with alpha 0 in
// every band `STEN` leaves empty.
struct MdecCodebook {
    // A flat 2^17 lookup over the entropy coder's 224 codewords: `(length << 16) | symbol`, 0 for
    // an undefined prefix. The three tables it is built from live in the EXE at guest 0x800528A0
    // (96 short descriptors), 0x80052A20 (128 long ones) and 0x80052C20 (the 224 default symbols);
    // docs\formats\video.md section 3 has the derivation. Nothing is hard-coded here: pass the
    // user's own `SLUS_010.53`.
    std::vector<uint32_t> lut;
    int maxLength = 0;
    bool Empty() const { return lut.empty(); }
    static MdecCodebook FromExe(std::span<const uint8_t> exe);
};

struct Panorama {
    static constexpr int kColumns = 110;
    static constexpr int kBands = 8;
    static constexpr int kTile = 16;
    static constexpr int kWidth = kColumns * kTile;  // 1760
    static constexpr int kHeight = kBands * kTile;   // 128

    ChunkHeader header;
    uint32_t road = 0;
    uint32_t distance = 0;           // where along that road this backdrop belongs
    std::vector<uint32_t> refs;      // the resource ids listed in the PANO header
    uint8_t code[kColumns * kBands] = {}; // the STEN 2-bit code of every (column, band)
    // `OFFS`, one byte per column: how many rows the column's tile stack is pushed DOWN when drawn.
    // `RASHCDG 0x800644F4` places column c's k-th tile at row `offs[c] + firstBand(c) + k` of a
    // grid whose row r sits at y = 279 r - 2232 on the radius-4096 ring (row 8 is eye height).
    uint8_t offs[kColumns] = {};
    bool haveOffs = false;
    int horzRecords = 0; // HORZ cut-out records applied (one per STEN code-2 tile)
    std::vector<uint8_t> rgba;       // kWidth * kHeight * 4, alpha 0 where `code` is 0
    int tiles = 0;                   // tiles actually decoded
};

Panorama ParsePanoramaChunk(std::span<const uint8_t> chunk, const MdecCodebook& book);

// World-space helpers. 1 world unit = 1/1024 of a raw road unit; positions are 16.16 fixed point.
inline float WorldX(const RoadSlice& s) { return static_cast<float>(s.pos[0]) / 65536.0f; }
inline float WorldY(const RoadSlice& s) { return static_cast<float>(s.pos[1]) / 65536.0f; }
inline float WorldZ(const RoadSlice& s) { return static_cast<float>(s.pos[2]) / 65536.0f; }

} // namespace rr
