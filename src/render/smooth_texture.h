#pragma once
// Smooth textures: the PS1's indexed images expanded, per palette row a
// primitive selects, into an RGBA texture array with mipmaps, so the renderer can filter them bilinearly and
// trilinearly. The expansion is made lazily, the first time a texture is bound with smoothing on, from the CPU copy
// UploadIndexedTexture registers - only for the palette rows the geometry uses when they were noted
// (NotePaletteRows), else for every row. Transparent texels (palette entry 0x0000) keep alpha 0 and take the colour
// of their opaque neighbours, so filtering does not darken the edges of cut-outs.
#include "render/gl_api.h"
#include "rrformats/model_texture.h"

#include <vector>

namespace rr::render {

struct SmoothTexture {
    GLuint array = 0;    // GL_TEXTURE_2D_ARRAY, RGBA8, one layer per expanded palette row, mipmapped
    GLuint layerMap = 0; // GL_TEXTURE_2D, R8, paletteCount x 1: layer + 1 of each row (0: not expanded)
    int layers = 0;
};

// The source of an uploaded indexed texture (gpu_texture.cpp).
void RegisterIndexedSource(GLuint indexTexture, const rr::IndexedTexture& source);
// The palette rows the geometry drawn with `indexTexture` selects (the rest are not expanded).
void NotePaletteRows(GLuint indexTexture, const std::vector<int>& rows);
// The smooth version of a texture, built on first use; nullptr when the texture was not registered.
const SmoothTexture* SmoothFor(GLuint indexTexture);
// Run totals for the log: textures expanded, layers, bytes.
std::string SmoothTotals();

} // namespace rr::render
