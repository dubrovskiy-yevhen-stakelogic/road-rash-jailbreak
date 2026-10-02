#pragma once
// A minimal PNG DECODER (png.h is the writer): non-interlaced images of every PNG colour type (grey, RGB, palette,
// grey + alpha, RGBA) at 1..16 bits per sample, to RGBA8, with its own raw DEFLATE decoder (RFC 1951). Used for the
// VR hands' albedo (third_party\vrhands, tools\rrgame\vr_hands_draw.h). Ported from the gt2-play project
// (src\gt2formats\png_reader.cpp and src\gt2vfs\inflate.cpp, MIT).
#include <cstdint>
#include <span>
#include <vector>

namespace rr {

struct PngImage {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba; // width * height * 4, rows top to bottom
};

// Throws std::runtime_error on malformed or unsupported input (Adam7-interlaced files are rejected).
PngImage DecodePng(std::span<const uint8_t> file);
// Raw DEFLATE; `consumed` (optional) receives the input bytes read.
std::vector<uint8_t> Inflate(std::span<const uint8_t> deflate, size_t* consumed = nullptr);

} // namespace rr
