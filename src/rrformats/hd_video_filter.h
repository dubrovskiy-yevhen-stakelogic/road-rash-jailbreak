#pragma once
// The films' bound on the neural enlargement (docs\HD-MEDIA.md), after the gt2-play project
// (src\gt2formats\hd_video_filter.h, MIT): every enlarged picture is pulled back towards a deterministic
// bicubic enlargement of ITS OWN original picture - the network may move a colour channel by at most 10 levels
// (half of a residual clamped to +-20) - so it sharpens but cannot invent detail, and no other picture is mixed in, so
// cuts and fast motion leave no trails.
//
// Temporal stability (ours): where the bicubic base of a pixel has not moved by more than `hold` levels since the
// output pixel was last produced, the previous output is kept. Still parts of a shot (a background, a title) then
// show the same enlarged pixels picture after picture instead of the network's picture-to-picture shimmer on the MDEC
// noise; anything that moves re-anchors at once. The anchor is the base the kept pixel was made from, not the previous
// picture's, so a slow fade cannot drift more than `hold` levels before the pixel is refreshed.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <vector>

namespace rr::hd {

// Bicubic (a = -0.5) enlargement of an RGBA picture to width x height (RGB, alpha ignored), as floats 0..255.
inline std::vector<float> BicubicRgb(const std::vector<uint8_t>& source, int sw, int sh, int width, int height) {
    const auto cubic = [](float x) {
        x = std::abs(x);
        return x <= 1 ? (1.5f * x - 2.5f) * x * x + 1 : x < 2 ? ((-0.5f * x + 2.5f) * x - 4) * x + 2 : 0;
    };
    struct Taps {
        int at[4];
        float weight[4];
    };
    const auto taps = [&](int count, int input) {
        std::vector<Taps> out(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) {
            const float p = (static_cast<float>(i) + 0.5f) * static_cast<float>(input) / static_cast<float>(count) - 0.5f;
            const int b = static_cast<int>(std::floor(p));
            for (int k = 0; k < 4; ++k) {
                out[static_cast<size_t>(i)].at[k] = std::clamp(b + k - 1, 0, input - 1);
                out[static_cast<size_t>(i)].weight[k] = cubic(p - static_cast<float>(b + k - 1));
            }
        }
        return out;
    };
    const auto xs = taps(width, sw), ys = taps(height, sh);
    std::vector<float> rows(static_cast<size_t>(width) * static_cast<size_t>(sh) * 3u);
    for (int y = 0; y < sh; ++y)
        for (int x = 0; x < width; ++x)
            for (int c = 0; c < 3; ++c) {
                float sum = 0;
                for (int k = 0; k < 4; ++k)
                    sum += xs[static_cast<size_t>(x)].weight[k] *
                           source[(static_cast<size_t>(y) * static_cast<size_t>(sw) + static_cast<size_t>(xs[static_cast<size_t>(x)].at[k])) * 4u + static_cast<size_t>(c)];
                rows[(static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 3u + static_cast<size_t>(c)] = sum;
            }
    std::vector<float> out(static_cast<size_t>(width) * static_cast<size_t>(height) * 3u);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            for (int c = 0; c < 3; ++c) {
                float base = 0;
                for (int k = 0; k < 4; ++k)
                    base += ys[static_cast<size_t>(y)].weight[k] *
                            rows[(static_cast<size_t>(ys[static_cast<size_t>(y)].at[k]) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 3u + static_cast<size_t>(c)];
                out[(static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 3u + static_cast<size_t>(c)] = std::clamp(base, 0.f, 255.f);
            }
    return out;
}

struct FilmStabiliser {
    int hold = 3;                 // levels
    std::vector<uint8_t> output;  // the previous result, RGB
    std::vector<float> anchor;    // the base each output pixel was made from
    size_t held = 0, total = 0;   // pixels kept / produced (for the report)
};

// `enhanced`: the network's picture (RGBA, width x height, modified in place); `source`: the original picture (RGBA,
// sw x sh). With `stab` null no pixel is held (the GT2 bound alone).
inline void ConstrainFilmPicture(std::vector<uint8_t>& enhanced, int width, int height, const std::vector<uint8_t>& source,
                                 int sw, int sh, FilmStabiliser* stab) {
    if (sw <= 0 || sh <= 0 || width < sw || height < sh || enhanced.size() != static_cast<size_t>(width) * static_cast<size_t>(height) * 4u ||
        source.size() != static_cast<size_t>(sw) * static_cast<size_t>(sh) * 4u)
        throw std::runtime_error("invalid film picture dimensions");
    const std::vector<float> base = BicubicRgb(source, sw, sh, width, height);
    const size_t n = static_cast<size_t>(width) * static_cast<size_t>(height);
    const bool haveOld = stab != nullptr && stab->output.size() == n * 3u;
    if (stab != nullptr && !haveOld) {
        stab->output.assign(n * 3u, 0);
        stab->anchor.assign(n * 3u, 0.f);
    }
    for (size_t i = 0; i < n; ++i) {
        const float* b = &base[i * 3u];
        if (haveOld) {
            const float* a = &stab->anchor[i * 3u];
            if (std::abs(b[0] - a[0]) <= static_cast<float>(stab->hold) && std::abs(b[1] - a[1]) <= static_cast<float>(stab->hold) &&
                std::abs(b[2] - a[2]) <= static_cast<float>(stab->hold)) {
                for (int c = 0; c < 3; ++c) enhanced[i * 4u + static_cast<size_t>(c)] = stab->output[i * 3u + static_cast<size_t>(c)];
                enhanced[i * 4u + 3u] = 255;
                ++stab->held;
                ++stab->total;
                continue;
            }
        }
        for (int c = 0; c < 3; ++c) {
            const float residual = std::clamp(static_cast<float>(enhanced[i * 4u + static_cast<size_t>(c)]) - b[c], -20.f, 20.f) * 0.5f;
            enhanced[i * 4u + static_cast<size_t>(c)] = static_cast<uint8_t>(std::clamp(std::lround(b[c] + residual), 0l, 255l));
        }
        enhanced[i * 4u + 3u] = 255;
        if (stab != nullptr) {
            for (int c = 0; c < 3; ++c) {
                stab->output[i * 3u + static_cast<size_t>(c)] = enhanced[i * 4u + static_cast<size_t>(c)];
                stab->anchor[i * 3u + static_cast<size_t>(c)] = b[c];
            }
            ++stab->total;
        }
    }
}

} // namespace rr::hd
