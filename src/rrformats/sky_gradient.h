#pragma once
// The sky ABOVE the skyline: the four-colour Gouraud gradient the original paints before anything
// else, and where its colours come from on the disc.
//
// Header-only on purpose: it adds no translation unit, so the build file does not have to change.
//
// WHAT IS ESTABLISHED:
//
//  * The upper sky is NOT in the type-4 panorama chunk. In `work\oracle\state\rr-race\ram.bin` the
//    resident panorama is empty in bands 0..3 in all 110 columns, yet the frame's upper half is
//    full of sky. What fills it is a set of `POLY_G4` quads emitted by `RASHCDG 0x80063C5C`
//    through the emitter at `RASHCDG 0x800642F8` (`len` 8, `code` 0x38), before the backdrop.
//  * The quad spans the whole screen from y = 0 down to a horizon line. The four corner points are
//    a four-entry `s16` array of screen (x, y) at guest `0x800523D0`, written by the same routine;
//    in all four captured states it reads (0,0) (384,0) (384,h) (0,h) with h = 91..94.
//  * Its colours come from four RGB entries at guest `0x800523E0`, `0x800523E8`, `0x800523F0` and
//    `0x800523F4` (the emitter is handed `0x800523F8` / `0x800523FC`, which hold copies of the
//    first two). The top of the sky takes entry A, the horizon entry B, and the middle band a
//    blend of entries C and D whose weight is an ANGLE: `RASHCDG 0x80063CC4` takes the camera's
//    yaw minus a per-level sun angle at `0x80052388`, forms the two screen-edge yaws by +-426 of
//    4096 (a 74.9 degree horizontal field of view), folds each into 0..2048 and scales it to
//    0..65536, and lerps C towards D with it. Left edge and right edge get their own weight, which
//    is why the middle of the sky changes colour across the screen.
//  * All four entries are copied at level load out of a 16-byte field in `DATA\GAMEBIN1.DAT`
//    (`kBundleStride` apart, at `kBundleOffset` inside each bundle). PROOF: the four colours live
//    in all four captured states are byte-identical to exactly ONE of the twelve bundles, and the
//    four vertex colours of the first primitive of the independently captured draw stream
//    `work\oracle\vr_capture\scene.csv` are that bundle's A and B exactly plus two blends of its
//    C and D.
//
// WHAT IS NOT: which bundle a given race selects. `rrview --skygrad <n>` picks it, as "which
// BBLEVEL*.TEX a level opens" is likewise left to the resident state.
#include <cstdint>
#include <span>
#include <stdexcept>

namespace rr {

struct SkyGradient {
    // 0 = A, the colour at the top of the sky; 1 = B, the colour at the horizon;
    // 2 = C and 3 = D, the two ends of the angle-dependent middle band.
    uint8_t rgb[4][3] = {};
    uint8_t tail[4] = {};   // the fourth byte of each entry; 0 or 0xE0 in every bundle on the disc
    size_t bundle = 0;
    bool valid = false;

    // One level bundle of `DATA\GAMEBIN1.DAT`. The size is the chunk size the file's own header
    // carries at +0x0C, and the first twelve bundles all hold this field.
    static constexpr size_t kBundleStride = 106612;
    static constexpr size_t kBundleOffset = 0x1145C;
    static constexpr size_t kBundleCount = 12;
};

inline size_t SkyGradientBundleCount(size_t fileSize) {
    const size_t whole = fileSize / SkyGradient::kBundleStride;
    return whole < SkyGradient::kBundleCount ? whole : SkyGradient::kBundleCount;
}

// The field at a known file offset. Since render7 the offset comes from the container itself
// (level_bundle.h: the payload of the bundle's type-1 section, which `RASHCDI 0x80061C24` hands to
// `0x80062384`): the fixed stride below holds only while every bundle before the wanted one is
// 106612 bytes long, and bundle 11 is 98456, so from bundle 12 on it lands in the wrong place.
inline SkyGradient ParseSkyGradientAt(std::span<const uint8_t> gamebin, size_t at, size_t bundle) {
    if (at + 16 > gamebin.size()) throw std::runtime_error("sky gradient: GAMEBIN1.DAT is too short");
    SkyGradient sky;
    sky.bundle = bundle;
    for (int entry = 0; entry < 4; ++entry) {
        for (int c = 0; c < 3; ++c) sky.rgb[entry][c] = gamebin[at + static_cast<size_t>(entry) * 4 + c];
        sky.tail[entry] = gamebin[at + static_cast<size_t>(entry) * 4 + 3];
    }
    sky.valid = true;
    for (int entry = 0; entry < 4; ++entry)
        if (sky.tail[entry] != 0x00 && sky.tail[entry] != 0xE0) sky.valid = false;
    return sky;
}

inline SkyGradient ParseSkyGradient(std::span<const uint8_t> gamebin, size_t bundle) {
    if (bundle >= SkyGradientBundleCount(gamebin.size()))
        throw std::runtime_error("sky gradient: bundle index is past the end of GAMEBIN1.DAT");
    const size_t at = bundle * SkyGradient::kBundleStride + SkyGradient::kBundleOffset;
    if (at + 16 > gamebin.size()) throw std::runtime_error("sky gradient: GAMEBIN1.DAT is too short");
    SkyGradient sky;
    sky.bundle = bundle;
    for (int entry = 0; entry < 4; ++entry) {
        for (int c = 0; c < 3; ++c) sky.rgb[entry][c] = gamebin[at + static_cast<size_t>(entry) * 4 + c];
        sky.tail[entry] = gamebin[at + static_cast<size_t>(entry) * 4 + 3];
    }
    // The field is its own check: the fourth byte of an entry is 0 or 0xE0 in every bundle, and a
    // stride that landed anywhere else would fail this immediately.
    sky.valid = true;
    for (int entry = 0; entry < 4; ++entry)
        if (sky.tail[entry] != 0x00 && sky.tail[entry] != 0xE0) sky.valid = false;
    return sky;
}

// The horizontal half-field the original uses when it samples the sky colour at the screen edges:
// `RASHCDG 0x80063CF4` forms `yaw - 426` and `0x80063D10` `yaw + 426`, in units of 4096 per turn.
inline constexpr int32_t kSkyEdgeAngle = 426;

// Fold an angle into 0..2048 and scale it to a 0..65536 blend weight, the way `RASHCDG 0x80063ED4`
// and `0x80063F0C` do: values past 2048 mirror back, then the result is shifted left by 5.
inline uint32_t SkyBlendWeight(int32_t angle) {
    int32_t folded = angle & 0xFFF;
    if (folded > 2048) folded = 4096 - folded;
    return static_cast<uint32_t>(folded) << 5;
}

// `weight` of entry D over entry C, in the original's own arithmetic: each channel is
// `(w * d + (65536 - w) * c) >> 16` with the two halves truncated separately, which is what the
// two calls to the fixed-point multiply at `0x8001FC90` add up to.
inline void SkyBlendColour(const SkyGradient& sky, uint32_t weight, uint8_t out[3]) {
    const uint32_t other = 65536u - (weight > 65536u ? 65536u : weight);
    for (int c = 0; c < 3; ++c) {
        const uint32_t a = (other * sky.rgb[2][c]) >> 16;
        const uint32_t b = ((weight > 65536u ? 65536u : weight) * sky.rgb[3][c]) >> 16;
        const uint32_t sum = a + b;
        out[c] = static_cast<uint8_t>(sum > 255 ? 255 : sum);
    }
}

} // namespace rr
