#pragma once
// An indexed texture on the GPU: the image and its palettes uploaded separately, the way the
// hardware keeps them, so one upload serves every palette a model's primitives select.
#include "render/gl_api.h"
#include "rrformats/model_texture.h"

namespace rr::render {

struct GpuIndexedTexture {
    GLuint indexTexture = 0;
    GLuint paletteTexture = 0;
    float width = 0, height = 0;
    float paletteCount = 0, paletteSize = 0;
    bool Valid() const { return indexTexture != 0; }
};

GpuIndexedTexture UploadIndexedTexture(const rr::IndexedTexture& source);

} // namespace rr::render
