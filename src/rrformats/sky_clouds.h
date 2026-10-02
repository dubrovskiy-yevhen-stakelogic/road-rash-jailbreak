#pragma once
// The CLOUD layer: the textured band the original paints over the sky gradient and under the type-4
// panorama. Header-only like sky_gradient.h and level_bundle.h.
//
// WHERE IT COMES FROM [proven from our own disassembly of RASHCDI.BIN sha1 9a8b79d8..., RASHCDG.BIN
// sha1 cfe43a77..., and byte-checked against the `rr-race` capture]:
//
//   * DATA\GAMEBIN1.DAT, the race's level bundle (level_bundle.h), section TYPE 3. The bundle
//     dispatcher `RASHCDI 0x80061C24` (jump table 0x8005B764, entry 3 -> 0x80061C90) hands the
//     payload to `RASHCDI 0x80060F58` with the race set (game_state+0x30).
//   * payload +0x04 s32 band height (1384), +0x08 s32 band top y (-1672), +0x0C u32, +0x10 u32,
//     all four copied to guest 0x800D43F8 / +4 / +0xC / +0x10; +0x14 a 4bpp TIM (256 x 63 texels
//     and a 16-colour CLUT). `0x80060F58` uploads the pixels to the rectangle the EXE's per-set
//     configuration table names in group 12 (`SLUS 0x800533B4` + 49/50, the rule
//     level_bundle.h `TextureConfigPoint` reads) and the CLUT 63 rows below it; in `rr-race`
//     that is pixels at VRAM (704, 192), CLUT at (704, 255).
//   * it then builds four UV slices (`0x80060780`, table 0x800D440C, 12 bytes each): slice k is
//     texels [64k, 64k + 63] (the last one 60 wide, u 192..251) by rows [0, 62] of the image, and
//     `0x80061100` a ring of 25 column pairs at 0x800D443C: point i at angle 170 i of 4096 (the
//     25th closing the ring at angle 0), x = sin, z = cos from the EXE's table 0x8005624C, top
//     y = +0x08, bottom y = +0x08 + +0x04. Radius 4096. Every value of both tables was read back
//     out of `work\oracle\state\rr-race\ram.bin` and matches.
//   * the loader sets the draw flag 0x8005B308 = 1. Two bundles carry no type-3 section (11 and
//     29 of the 30), and there the layer is simply absent.
//
// HOW IT IS DRAWN, `RASHCDG 0x8006396C` (called from 0x80064B9C when 0x8005B308 != 0):
//   * TR = 0 (0x80064B9C clears it) and RT = the view record's camera matrix (+0x5C), so the ring
//     is infinitely far: only the camera's rotation moves it.
//   * start segment s = (yawColumn * 24) / 110 with yawColumn = (((yaw - 0x1BB) & 0xFFF) * 110) >> 12,
//     the same column the panorama starts from; eight consecutive segments s..s+7 (mod 24).
//   * segment j: POLY_FT4, code 0x2F (raw texture, semi-transparent), corners top(j), top(j+1),
//     bottom(j), bottom(j+1), UVs of slice j & 3; culled when both of its edge columns project left
//     of x = 0 or both right of x = 384.
//   * the CLUT's entries all carry the STP bit, so every non-zero texel is blended B/2 + F/2
//     (tpage semi-transparency mode 0); entry 0 (0x0000) is transparent.
#include "rrformats/level_bundle.h"

#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace rr {

struct SkyClouds {
    bool valid = false;
    size_t bundle = 0;
    int32_t height = 0;  // payload +0x04
    int32_t top = 0;     // payload +0x08
    uint32_t field0C = 0, field10 = 0;
    int width = 0, rows = 0;         // texels
    uint16_t timX = 0, timY = 0;     // the rectangle the TIM itself names (the loader ignores it)
    std::vector<uint8_t> indices;    // width * rows, 4-bit indices
    std::array<uint16_t, 16> clut{};

    static constexpr int kSegments = 24;
    static constexpr int kDrawn = 8;
    static constexpr int kRadius = 4096;
};

// One UV slice as `RASHCDI 0x80060780` builds it: u0..u1, v0..v1 in texels of the image.
struct SkyCloudSlice {
    int u0 = 0, u1 = 0, v0 = 0, v1 = 0;
};

inline SkyCloudSlice SkyCloudSliceOf(int segment) {
    const int k = segment & 3;
    SkyCloudSlice s;
    s.u0 = 64 * k;
    s.u1 = s.u0 + (k == 3 ? 15 : 16) * 4 - 1; // `0x80060F58` passes 16 halfwords, 15 for the last
    s.v0 = 0;
    s.v1 = 62;                                // and 62 rows
    return s;
}

// Angle of ring point i, `RASHCDI 0x80061100`: an accumulator stepping by 0xAA00 (170.67 << 8),
// reset to 0 for the 25th point.
inline uint32_t SkyCloudAngle(int point) {
    if (point >= SkyClouds::kSegments) return 0;
    return (static_cast<uint32_t>(point) * 0xAA00u >> 8) & 0xFFFu;
}

// The first of the eight segments the original draws, `RASHCDG 0x8006396C` / `0x800650D0`.
inline int SkyCloudFirstSegment(int32_t cameraYaw) {
    const int32_t column = static_cast<int32_t>((static_cast<uint32_t>(cameraYaw - 0x1BB) & 0xFFFu) * 110u >> 12);
    return (column * 24) / 110;
}

inline SkyClouds ParseSkyClouds(std::span<const uint8_t> file, const LevelBundle& bundle) {
    using namespace level_bundle_detail;
    SkyClouds c;
    c.bundle = bundle.index;
    const LevelBundleSection* section = bundle.Find(3);
    if (!section) return c; // bundles 11 and 29 carry none: no cloud layer
    const size_t p = section->payload;
    c.height = static_cast<int32_t>(U32(file, p + 4));
    c.top = static_cast<int32_t>(U32(file, p + 8));
    c.field0C = U32(file, p + 0x0C);
    c.field10 = U32(file, p + 0x10);
    const size_t tim = p + 0x14;
    if (U32(file, tim) != 0x10) throw std::runtime_error("GAMEBIN1.DAT: cloud TIM magic");
    const uint32_t flags = U32(file, tim + 4);
    if ((flags & 3) != 0 || (flags & 8) == 0) throw std::runtime_error("GAMEBIN1.DAT: cloud TIM is not 4bpp with a CLUT");
    const size_t clutBlock = tim + 8;
    const uint32_t clutBytes = U32(file, clutBlock);
    if (U16(file, clutBlock + 8) < 16 || clutBytes < 12 + 32) throw std::runtime_error("GAMEBIN1.DAT: cloud CLUT is short");
    for (size_t i = 0; i < 16; ++i) c.clut[i] = U16(file, clutBlock + 12 + i * 2);
    const size_t pix = clutBlock + clutBytes;
    const uint32_t bnum = U32(file, pix);
    c.timX = U16(file, pix + 4);
    c.timY = U16(file, pix + 6);
    const size_t wHalf = U16(file, pix + 8);
    c.rows = U16(file, pix + 10);
    c.width = static_cast<int>(wHalf * 4);
    if (bnum != 12 + wHalf * static_cast<size_t>(c.rows) * 2 || pix + bnum > section->limit)
        throw std::runtime_error("GAMEBIN1.DAT: cloud TIM pixel block does not add up");
    c.indices.resize(static_cast<size_t>(c.width) * static_cast<size_t>(c.rows));
    for (int y = 0; y < c.rows; ++y)
        for (int x = 0; x < c.width; ++x) {
            const uint8_t byte = file[pix + 12 + static_cast<size_t>(y) * wHalf * 2 + static_cast<size_t>(x / 2)];
            c.indices[static_cast<size_t>(y) * static_cast<size_t>(c.width) + static_cast<size_t>(x)] =
                static_cast<uint8_t>((x & 1) ? (byte >> 4) : (byte & 0xF));
        }
    c.valid = c.width >= 256 && c.rows >= 63 && c.height > 0;
    return c;
}

} // namespace rr
