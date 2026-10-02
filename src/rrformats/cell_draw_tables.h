#pragma once
// The constant tables the original's scene-cell draw routines read, taken out of the player's own
// RASHCDG.BIN and SLUS_010.53 at run time (nothing of them is stored in this repository).
//
// Header-only on purpose (like sky_gradient.h): it adds no translation unit.
//
// Every table is named by the guest address the draw code reads it at. RASHCDG.BIN is loaded at
// 0x8005B5E8 (sha1 cfe43a7786759f2cb9c57751cf99e84d1074782c) and all of these lie inside its image,
// so the file offset is `address - 0x8005B5E8`; in `rr-race` each one is byte-identical to the
// file. The reading of each table is in docs\formats\scene_cell.md 13.
#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>

namespace rr {

// A GP0(E2) texture window, decoded: inside the window only the low bits of a texel coordinate
// survive and the tile origin is ORed in, i.e. `t' = (t & (size - 1)) | origin` for the power-of-two
// tile sizes this game uses. size 0 = no window.
struct TextureWindow {
    int x = 0, y = 0, width = 0, height = 0;
};
inline TextureWindow DecodeTextureWindow(uint32_t word) {
    TextureWindow w;
    if ((word >> 24) != 0xE2) return w;
    const uint32_t maskX = word & 31, maskY = (word >> 5) & 31, offX = (word >> 10) & 31, offY = (word >> 15) & 31;
    if (maskX == 0 && maskY == 0) return w;
    const uint32_t bitsX = maskX * 8, bitsY = maskY * 8;
    w.width = static_cast<int>(bitsX & (~bitsX + 1)); // the lowest replaced bit is the tile size
    w.height = static_cast<int>(bitsY & (~bitsY + 1));
    w.x = static_cast<int>((offX & maskX) * 8);
    w.y = static_cast<int>((offY & maskY) * 8);
    return w;
}

struct CellDrawTables {
    // `RASHCDG 0x800CC994`, 32 words: band 0's texture windows, indexed by the FIXED-UP flags byte
    // `>> 3` (`0x8006DAFC..0x8006DB10`). The load-time fix-up `SLUS 0x80033EA0` rewrites a disc
    // flags byte `t << 4 | low` (t < 15) into `(t + 15 * (page in the lower half)) << 3 | low`, and
    // t = 15 into 30 (no window, 0xE2000000), so entries 0..14 are 32 x 32 tiles of the image's own
    // 256 x 128 and 15..29 the same tiles 128 rows down, in the VRAM page.
    std::array<uint32_t, 32> band0Window{};
    // `RASHCDG 0x800CC954`, 16 words: band 1's texture windows, copied to scratchpad 0x1F800078 by
    // `0x80068FF0..0x80069018` and indexed by the (not fixed-up) flags byte `>> 4`
    // (`0x8006CBBC..0x8006CBDC`): 64 x 64 tiles, entry 15 none.
    std::array<uint32_t, 16> band1Window{};
    // `RASHCDG 0x800CC874`, 16 words, indexed by `attr & 0xF` of a band-2 quad (`0x8006E9A8..`):
    // 5 bits per lane strip - bits 0..1 the UV template kind, bits 2..3 the palette offset.
    std::array<uint32_t, 16> stripKinds{};
    // `RASHCDG 0x800CC914`, 16 words, same index: bits 28..31 the number of lane strips, and 2 bits
    // per strip edge naming a lane-line colour (0 = no line).
    std::array<uint32_t, 16> stripLines{};
    // `RASHCDG 0x800CC8B4`, 8 records of 16 bytes: the four corner UVs (u | v << 8, at +0/+4/+8/+12)
    // of template `2 * kind + near` (`0x8006F460..0x8006F4F0`).
    std::array<std::array<uint16_t, 4>, 8> stripUv{};
    // `RASHCDG 0x800CCA58`, indexed by `|maxZ| / 512` of the quad (`0x8006E96C..0x8006E9EC`): 1 when
    // the quad is near enough for the full-size road texture. Only indices below 8 are reachable
    // on the near path (maxZ < 4096).
    std::array<uint8_t, 8> nearByDepth{};
    // `SLUS 0x800523AC`, 4 words: the lane-line colours (`0x8006EEF0..0x8006EF04`).
    std::array<uint32_t, 4> lineColour{};
    // `RASHCDG 0x800CCA30` / `0x800CCA40` / `0x800CCA50`: the child tables of the triangle, quad and line
    // subdividers (`0x8006929C`, `0x80069784` / `0x80069CF0`, `0x8006A25C`; scene_cell.md 13.8), one word per
    // child whose nibbles name its vertices among the corners and the new midpoints.
    std::array<uint32_t, 4> subTri{};
    std::array<uint32_t, 4> subQuad{};
    std::array<uint32_t, 2> subLine{};
};

namespace cell_draw_detail {
inline uint32_t U32(std::span<const uint8_t> d, size_t at) {
    if (at + 4 > d.size()) throw std::runtime_error("cell draw tables: read past the end of the file");
    return static_cast<uint32_t>(d[at]) | (static_cast<uint32_t>(d[at + 1]) << 8) |
           (static_cast<uint32_t>(d[at + 2]) << 16) | (static_cast<uint32_t>(d[at + 3]) << 24);
}
} // namespace cell_draw_detail

inline CellDrawTables ReadCellDrawTables(std::span<const uint8_t> overlay, std::span<const uint8_t> exe) {
    using cell_draw_detail::U32;
    constexpr uint32_t kOverlayBase = 0x8005B5E8u;
    const auto at = [&](uint32_t address) { return static_cast<size_t>(address - kOverlayBase); };
    CellDrawTables t;
    for (size_t i = 0; i < 32; ++i) t.band0Window[i] = U32(overlay, at(0x800CC994u) + i * 4);
    for (size_t i = 0; i < 16; ++i) t.band1Window[i] = U32(overlay, at(0x800CC954u) + i * 4);
    for (size_t i = 0; i < 16; ++i) t.stripKinds[i] = U32(overlay, at(0x800CC874u) + i * 4);
    for (size_t i = 0; i < 16; ++i) t.stripLines[i] = U32(overlay, at(0x800CC914u) + i * 4);
    for (size_t e = 0; e < 8; ++e)
        for (size_t c = 0; c < 4; ++c)
            t.stripUv[e][c] = static_cast<uint16_t>(U32(overlay, at(0x800CC8B4u) + e * 16 + c * 4) & 0xFFFF);
    for (size_t i = 0; i < 8; ++i) t.nearByDepth[i] = overlay[at(0x800CCA58u) + i];
    for (size_t i = 0; i < 4; ++i) t.subTri[i] = U32(overlay, at(0x800CCA30u) + i * 4);
    for (size_t i = 0; i < 4; ++i) t.subQuad[i] = U32(overlay, at(0x800CCA40u) + i * 4);
    for (size_t i = 0; i < 2; ++i) t.subLine[i] = U32(overlay, at(0x800CCA50u) + i * 4);
    // The EXE: text address 0x80010000 at file offset 0x800.
    for (size_t i = 0; i < 4; ++i) t.lineColour[i] = U32(exe, 0x800523ACu - 0x80010000u + 0x800 + i * 4) & 0x00FFFFFFu;
    // The structure is its own check: every window word is an E2 command or 0, and every strip count
    // is 1..6 - a wrong base address would fail this at once.
    for (uint32_t w : t.band0Window)
        if (w != 0 && (w >> 24) != 0xE2) throw std::runtime_error("cell draw tables: band-0 window table is not E2 words");
    for (uint32_t w : t.band1Window)
        if (w != 0 && (w >> 24) != 0xE2) throw std::runtime_error("cell draw tables: band-1 window table is not E2 words");
    for (uint32_t w : t.stripLines)
        if ((w >> 28) == 0 || (w >> 28) > 7) throw std::runtime_error("cell draw tables: strip count out of range");
    return t;
}

} // namespace rr
