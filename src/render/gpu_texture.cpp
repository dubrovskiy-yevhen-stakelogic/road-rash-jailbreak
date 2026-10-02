#include "render/gpu_texture.h"

#include "render/smooth_texture.h" // the smooth textures' CPU copy

#include <cstdio>

namespace rr::render {

GpuIndexedTexture UploadIndexedTexture(const rr::IndexedTexture& source) {
    GpuIndexedTexture out;
    if (source.Empty()) return out;
    out.width = static_cast<float>(source.width);
    out.height = static_cast<float>(source.height);
    out.paletteCount = static_cast<float>(source.paletteCount);
    out.paletteSize = static_cast<float>(source.paletteSize);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    while (glGetError() != GL_NO_ERROR) {
    } // drain anything left by earlier calls, so the next check is about this upload
    glGenTextures(1, &out.indexTexture);
    glBindTexture(GL_TEXTURE_2D, out.indexTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, source.width, source.height, 0, GL_RED, GL_UNSIGNED_BYTE,
                 source.indices.data());
    if (const GLenum error = glGetError(); error != GL_NO_ERROR)
        std::fprintf(stderr, "index texture upload failed: GL error 0x%04X (%dx%d)\n", error, source.width,
                     source.height);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &out.paletteTexture);
    glBindTexture(GL_TEXTURE_2D, out.paletteTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, source.paletteSize, source.paletteCount, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, source.palettes.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    RegisterIndexedSource(out.indexTexture, source); // expanded only if smooth textures are ever drawn
    return out;
}

} // namespace rr::render
