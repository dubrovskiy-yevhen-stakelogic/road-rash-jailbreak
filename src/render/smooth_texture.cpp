#include "render/smooth_texture.h"

#include "render/render_target.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string>

namespace rr::render {

namespace {

constexpr GLenum kTexture2DArray = 0x8C1A, kRgba8 = 0x8058, kMaxLevel = 0x813D, kMaxAnisotropy = 0x84FE,
                 kMaxMaxAnisotropy = 0x84FF, kLinearMipmapLinear = 0x2703;

struct Source {
    rr::IndexedTexture image;
    std::set<int> rows; // empty: every row
    SmoothTexture smooth;
    bool built = false;
};

std::map<GLuint, Source>& Sources() {
    static std::map<GLuint, Source> s;
    return s;
}

size_t g_textures = 0, g_layers = 0, g_bytes = 0;

// One palette row of the image as RGBA8, the transparent texels' colour taken from their opaque neighbours.
std::vector<uint32_t> Expand(const rr::IndexedTexture& t, int row) {
    const size_t w = static_cast<size_t>(t.width), h = static_cast<size_t>(t.height);
    std::vector<uint32_t> out(w * h);
    const size_t base = static_cast<size_t>(row) * static_cast<size_t>(t.paletteSize);
    for (size_t i = 0; i < out.size(); ++i) {
        const size_t e = base + t.indices[i];
        uint32_t c = e < t.palettes.size() ? t.palettes[e] : 0u;
        c = (c >> 24) >= 128 ? (c | 0xFF000000u) : (c & 0x00FFFFFFu); // alpha 0 or 255 (the shader's texel.a < 0.5)
        out[i] = c;
    }
    // two passes of dilation into the transparent texels (their alpha stays 0)
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<uint32_t> next = out;
        for (size_t y = 0; y < h; ++y)
            for (size_t x = 0; x < w; ++x) {
                const size_t i = y * w + x;
                if ((out[i] >> 24) != 0) continue;
                uint32_t sum[3] = {0, 0, 0}, n = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const long xx = static_cast<long>(x) + dx, yy = static_cast<long>(y) + dy;
                        if (xx < 0 || yy < 0 || xx >= static_cast<long>(w) || yy >= static_cast<long>(h)) continue;
                        const uint32_t c = out[static_cast<size_t>(yy) * w + static_cast<size_t>(xx)];
                        if ((c >> 24) == 0 && pass == 0) continue;
                        if ((c & 0x00FFFFFFu) == 0 && (c >> 24) == 0) continue;
                        for (int k = 0; k < 3; ++k) sum[k] += (c >> (8 * k)) & 0xFFu;
                        ++n;
                    }
                if (n > 0) next[i] = (sum[0] / n) | ((sum[1] / n) << 8) | ((sum[2] / n) << 16);
            }
        out.swap(next);
    }
    return out;
}

void Build(Source& s) {
    s.built = true;
    const rr::IndexedTexture& t = s.image;
    if (t.Empty() || t.paletteCount <= 0 || t.width <= 0 || t.height <= 0) return;
    std::vector<int> rows;
    for (int r = 0; r < t.paletteCount && r < 255; ++r)
        if (s.rows.empty() || s.rows.count(r)) rows.push_back(r);
    if (rows.empty()) return;
    const size_t layerTexels = static_cast<size_t>(t.width) * static_cast<size_t>(t.height);
    std::vector<uint32_t> all(layerTexels * rows.size());
    for (size_t k = 0; k < rows.size(); ++k) {
        const std::vector<uint32_t> one = Expand(t, rows[k]);
        std::copy(one.begin(), one.end(), all.begin() + static_cast<std::ptrdiff_t>(k * layerTexels));
    }
    GLint unit = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &unit);
    gl.ActiveTexture(GL_TEXTURE0 + 7); // a unit nothing else binds (the scene's are 0..6)
    glGenTextures(1, &s.smooth.array);
    glBindTexture(kTexture2DArray, s.smooth.array);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    TexImage3D(kTexture2DArray, 0, static_cast<int>(kRgba8), t.width, t.height, static_cast<int>(rows.size()), GL_RGBA,
               GL_UNSIGNED_BYTE, all.data());
    glTexParameteri(kTexture2DArray, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(kTexture2DArray, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(kTexture2DArray, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(kTexture2DArray, GL_TEXTURE_MIN_FILTER, kLinearMipmapLinear);
    // an atlas: past 1/16 the neighbouring images bleed in, so the chain stops there
    glTexParameteri(kTexture2DArray, kMaxLevel, 4);
    GLfloat maxAniso = 0.0f;
    while (glGetError() != GL_NO_ERROR) {
    }
    glGetFloatv(kMaxMaxAnisotropy, &maxAniso);
    if (glGetError() == GL_NO_ERROR && maxAniso >= 2.0f) glTexParameterf(kTexture2DArray, kMaxAnisotropy, std::min(8.0f, maxAniso));
    GenerateMipmap(kTexture2DArray);
    std::vector<uint8_t> map(static_cast<size_t>(t.paletteCount), 0);
    for (size_t k = 0; k < rows.size(); ++k) map[static_cast<size_t>(rows[k])] = static_cast<uint8_t>(k + 1);
    glGenTextures(1, &s.smooth.layerMap);
    glBindTexture(GL_TEXTURE_2D, s.smooth.layerMap);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, t.paletteCount, 1, 0, GL_RED, GL_UNSIGNED_BYTE, map.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.ActiveTexture(static_cast<GLenum>(unit));
    s.smooth.layers = static_cast<int>(rows.size());
    ++g_textures;
    g_layers += rows.size();
    g_bytes += all.size() * 4u * 4u / 3u;
}

} // namespace

void RegisterIndexedSource(GLuint indexTexture, const rr::IndexedTexture& source) {
    if (indexTexture == 0) return;
    Source& s = Sources()[indexTexture];
    s.image = source;
    s.built = false;
}

void NotePaletteRows(GLuint indexTexture, const std::vector<int>& rows) {
    const auto it = Sources().find(indexTexture);
    if (it == Sources().end()) return;
    for (int r : rows) it->second.rows.insert(r);
}

const SmoothTexture* SmoothFor(GLuint indexTexture) {
    const auto it = Sources().find(indexTexture);
    if (it == Sources().end()) return nullptr;
    if (!it->second.built) Build(it->second);
    return it->second.smooth.array != 0 ? &it->second.smooth : nullptr;
}

std::string SmoothTotals() {
    char b[200];
    std::snprintf(b, sizeof(b), "smooth textures: %zu texture(s) expanded, %zu palette layer(s), %.1f MB with mipmaps",
                  g_textures, g_layers, static_cast<double>(g_bytes) / (1024.0 * 1024.0));
    return b;
}

} // namespace rr::render
