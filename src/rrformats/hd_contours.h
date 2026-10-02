#pragma once
// The contour pass of the HD preparation (docs\HD-MEDIA.md): an offline CPU adaptation of Hyllian's xBR-lv3 edge rules
// (MIT, third_party\xbr) to 4-bit PALETTE INDICES, as the gt2-play project does it
// (src\gt2formats\ui_contours.h, MIT). Each source texel becomes 4 x 4 texels that hold two original
// indices - its own and the neighbour across the contour - and the weight of the neighbour, so the game still colours
// them with the live palette. The luma the edge rules compare comes from the region's palette (`palette`, BGR555,
// 0x0000 = transparent), only at preparation time.
//
// A region is processed on its own: texels outside it are read as index 0 (transparent, `outsideZero`: a font glyph -
// the glyph's quad ends at its box) or as the nearest texel inside it (a HUD art rectangle, of which the packets may
// draw any part), so neighbouring art of another CLUT never bleeds in.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace rr::hd {

// `indices` is an image `stride` texels wide; the region (rx, ry, rw, rh) is enlarged into `out` (rw*4 x rh*4 texels,
// 3 bytes each: a, b, weight), written at `out` + ((oy * outStride) + ox) * 3.
inline void SmoothContours4x(const uint8_t* indices, int stride, int rows, int rx, int ry, int rw, int rh,
                             const std::array<uint16_t, 16>& palette, bool outsideZero, uint8_t* out, int outStride,
                             int ox, int oy) {
    if (rw <= 0 || rh <= 0 || rx < 0 || ry < 0 || rx + rw > stride || ry + rh > rows) throw std::runtime_error("invalid contour region");
    std::array<float, 16> luma{};
    std::array<std::array<float, 3>, 16> rgb{};
    for (int i = 0; i < 16; ++i) {
        for (int c = 0; c < 3; ++c) rgb[static_cast<size_t>(i)][static_cast<size_t>(c)] = static_cast<float>((palette[static_cast<size_t>(i)] >> (5 * c)) & 31) / 31.f;
        const auto& p = rgb[static_cast<size_t>(i)];
        luma[static_cast<size_t>(i)] = (palette[static_cast<size_t>(i)] & 0x7FFFu) || (palette[static_cast<size_t>(i)] & 0x8000u)
                                           ? 48.f * (.299f * p[0] + .587f * p[1] + .114f * p[2])
                                           : -48.f;
    }
    const auto at = [&](int x, int y) -> int {
        if (x < rx || y < ry || x >= rx + rw || y >= ry + rh) {
            if (outsideZero) return 0;
            x = std::clamp(x, rx, rx + rw - 1);
            y = std::clamp(y, ry, ry + rh - 1);
        }
        const int v = indices[static_cast<size_t>(y) * static_cast<size_t>(stride) + static_cast<size_t>(x)];
        if (v > 15) throw std::runtime_error("contour index above 15");
        return v;
    };
    const auto ramp = [](float value, float centre) {
        const float t = std::clamp((value - centre + .4f) / .8f, 0.f, 1.f);
        return t * t * (3 - 2 * t);
    };
    for (int y = ry; y < ry + rh; ++y)
        for (int x = rx; x < rx + rw; ++x) {
            const int centre = at(x, y);
            struct Edge {
                bool active = false, left = false, up = false, left3 = false, up3 = false;
                int next = 0;
                float distance = 0;
            };
            Edge edges[4];
            for (int rotation = 0; rotation < 4; ++rotation) {
                const auto index = [&](int dx, int dy) {
                    for (int r = 0; r < rotation; ++r) {
                        const int old = dx;
                        dx = dy;
                        dy = -old;
                    }
                    return at(x + dx, y + dy);
                };
                const auto sample = [&](int dx, int dy) { return luma[static_cast<size_t>(index(dx, dy))]; };
                const float e = luma[static_cast<size_t>(centre)], b = sample(0, -1), c = sample(1, -1), d = sample(-1, 0),
                            f = sample(1, 0), g = sample(-1, 1), h = sample(0, 1), i = sample(1, 1), f4 = sample(2, 0),
                            h5 = sample(0, 2), i4 = sample(2, 1), i5 = sample(1, 2), c1 = sample(1, -2), g0 = sample(-2, 1),
                            b1 = sample(0, -2), d0 = sample(-2, 0);
                const auto eq = [](float p, float q) { return std::abs(p - q) < 10.f; };
                const bool restriction = e != f && e != h &&
                                         ((!eq(f, b) && !eq(f, c)) || (!eq(h, d) && !eq(h, g)) ||
                                          (eq(e, i) && ((!eq(f, f4) && !eq(f, i4)) || (!eq(h, h5) && !eq(h, i5)))) ||
                                          eq(e, g) || eq(e, c));
                const float diagonal = std::abs(e - c) + std::abs(e - g) + std::abs(i - h5) + std::abs(i - f4) + 4 * std::abs(h - f);
                const float cross = std::abs(h - d) + std::abs(h - i5) + std::abs(f - i4) + std::abs(f - b) + 4 * std::abs(e - i);
                Edge& edge = edges[rotation];
                edge.active = restriction && diagonal < cross;
                edge.left = 2 * std::abs(f - g) <= std::abs(h - c) && e != g && d != g;
                edge.up = std::abs(f - g) >= 2 * std::abs(h - c) && e != c && b != c;
                edge.left3 = std::abs(g - g0) < 2 && std::abs(d0 - g0) >= 2;
                edge.up3 = std::abs(c - c1) < 2 && std::abs(b1 - c1) >= 2;
                edge.next = std::abs(e - f) <= std::abs(e - h) ? index(1, 0) : index(0, 1);
                for (int ch = 0; ch < 3; ++ch)
                    edge.distance += std::abs(rgb[static_cast<size_t>(centre)][static_cast<size_t>(ch)] -
                                              rgb[static_cast<size_t>(edge.next)][static_cast<size_t>(ch)]);
                if (!palette[static_cast<size_t>(centre)] != !palette[static_cast<size_t>(edge.next)]) edge.distance += 3;
            }
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx) {
                    float best = -1, weight = 0;
                    int next = centre;
                    for (int rotation = 0; rotation < 4; ++rotation) {
                        const Edge& edge = edges[rotation];
                        if (!edge.active) continue;
                        float u = (static_cast<float>(sx) + .5f) / 4.f, v = (static_cast<float>(sy) + .5f) / 4.f;
                        for (int r = 0; r < rotation; ++r) {
                            const float old = u;
                            u = 1 - v;
                            v = old;
                        }
                        float w = ramp(v + u, 1.5f);
                        if (edge.left) {
                            w = (std::max)(w, ramp(v + .5f * u, 1.f));
                            if (edge.left3) w = (std::max)(w, ramp(6 * v + 2 * u, 5.f));
                        }
                        if (edge.up) {
                            w = (std::max)(w, ramp(v + 2 * u, 2.f));
                            if (edge.up3) w = (std::max)(w, ramp(2 * v + 6 * u, 5.f));
                        }
                        if (w * edge.distance > best) {
                            best = w * edge.distance;
                            weight = w;
                            next = edge.next;
                        }
                    }
                    const size_t o = (static_cast<size_t>(oy + (y - ry) * 4 + sy) * static_cast<size_t>(outStride) +
                                      static_cast<size_t>(ox + (x - rx) * 4 + sx)) * 3u;
                    out[o] = static_cast<uint8_t>(centre);
                    out[o + 1] = static_cast<uint8_t>(next);
                    out[o + 2] = static_cast<uint8_t>(weight * 255 + .5f);
                }
        }
}

// The plain 4x of a region (every texel its own index, weight 0): what a texel outside every contoured region gets.
inline void NearestContours4x(const uint8_t* indices, int stride, int rx, int ry, int rw, int rh, uint8_t* out,
                              int outStride, int ox, int oy) {
    for (int y = 0; y < rh * 4; ++y)
        for (int x = 0; x < rw * 4; ++x) {
            const uint8_t v = indices[static_cast<size_t>(ry + y / 4) * static_cast<size_t>(stride) + static_cast<size_t>(rx + x / 4)];
            const size_t o = (static_cast<size_t>(oy + y) * static_cast<size_t>(outStride) + static_cast<size_t>(ox + x)) * 3u;
            out[o] = v;
            out[o + 1] = v;
            out[o + 2] = 0;
        }
}

} // namespace rr::hd
