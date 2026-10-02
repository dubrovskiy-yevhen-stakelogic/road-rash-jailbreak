#pragma once
// Texture containers of Road Rash: Jailbreak.
//
// `*.TEX` and `*.MRO` are a generic chain of `char tag[4]; u32 size;` chunks - the same shape as
// every other container in this game. `LECT` chunks carry the images; the chain also holds `CTKP`,
// `TSLP`, `KNBP` and even `RMD3`/`DMD3` models. Layout proven in docs\formats\textures.md; this is
// the authoritative parser (the Python probe tools\scout\tex.py stays as an independent cross-check).
#include <cstdint>
#include <span>
#include <vector>

namespace rr {

struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba; // width*height*4, top row first
    bool Empty() const { return rgba.empty(); }
};

// One LECT image. Kinds 1/2/3 embed a complete PS1 TIM and are self-contained. Kinds 5/6 are raw
// indexed pixels with NO CLUT in the file: the engine parks their palettes in the spare rows of the
// atlas's own VRAM page column and the per-primitive CLUT id picks a row, so those need a palette
// supplied from outside (see docs\formats\textures.md section 5a).
struct TextureChunk {
    uint16_t id = 0;
    uint8_t kind = 0;
    uint8_t bpp = 0;
    uint8_t slot = 0;
    int width = 0;
    int height = 0;
    bool hasPalette = false;
    std::vector<uint8_t> indices; // one byte per pixel, already un-nibbled for 4bpp
    std::vector<uint32_t> palette; // RGBA, empty for kinds 5/6
    Image decoded;                 // empty when the palette is not in the file
};

// Walks the whole chain and returns every LECT image in it. Non-LECT chunks are skipped, not an error.
std::vector<TextureChunk> ParseTextureContainer(std::span<const uint8_t> data);

// Colours an index image with an externally supplied 16- or 256-entry palette.
Image ApplyPalette(const TextureChunk& chunk, std::span<const uint32_t> palette);

// PS1 BGR555 halfword -> RGBA. Colour 0 is the transparent index by PS1 convention; the STP bit
// selects a blend equation at draw time, so it is reported rather than baked into alpha.
uint32_t Bgr555ToRgba(uint16_t texel, bool* semiTransparent = nullptr);

} // namespace rr
