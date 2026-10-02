#pragma once
// The race HUD in HD (docs\HD-MEDIA.md): the same packets HudFrame links (hud_view.h), rasterised `scale` times larger
// (384 * scale x 240 * scale, premultiplied RGBA8). A textured pixel inside a region of the HD pack's HUD atlases
// (hd_pack.h kind "hud": an art rectangle or the HUD font with the CLUT it is drawn with) takes the region's 4x
// CONTOUR texel - two original indices and a weight - through the LIVE CLUT of the page, so every colour, flash and
// semi-transparency rule stays the original's; anything else is the original texel enlarged. Flat tiles, polygons and
// lines are drawn at the finer grid (their edges no longer step in 4-pixel blocks) with the original's colours.
//
// A region is used only while the page's texels and CLUT in it hash to the entry's source hash (Prepare), so a pack
// made from another disc or another HUD layout never draws.
//
// The 1x path (hud_view.cpp RasterizeHud) is untouched: with HD off the HUD is byte for byte today's.
#include "game/hud_view.h"
#include "rrformats/hd_pack.h"

#include <memory>
#include <string>
#include <vector>

namespace rr::game {

class HudHd {
public:
    HudHd() = default;
    HudHd(const HudHd&) = delete;
    HudHd& operator=(const HudHd&) = delete;
    ~HudHd(); // prints Report() once when it drew anything (the run's log)
    // Re-checks the active pack's HUD regions against `vram` when the pack, the HD switch or the page changed. True when
    // at least one region matches (the caller may still draw HD without: tiles and lines gain the finer grid).
    bool Prepare(const HudVram& vram);
    // Draws `packets` over `out` (cleared here to transparent) at `scale` (2 or 4): out is (384 * scale) x
    // (240 * scale) x 4 bytes, premultiplied.
    HudRasterStats Rasterize(const std::vector<HudPacket>& packets, const HudVram& vram, int32_t originX, int32_t originY,
                             int scale, std::vector<uint8_t>& out);
    // One line: regions matched / offered, pixels drawn from contours.
    std::string Report() const;

    struct Region {
        int u = 0, v = 0, w = 0, h = 0;
        uint16_t clut = 0;
        std::shared_ptr<const rr::hd::Contour> atlas;
    };

private:
    std::vector<Region> regions_;
    uint64_t generation_ = 0, pageHash_ = 0;
    size_t offered_ = 0;
    uint64_t contourPixels_ = 0, frames_ = 0;
    std::vector<std::pair<std::string, std::shared_ptr<const rr::hd::Contour>>> loaded_; // file -> atlas, across races
};

} // namespace rr::game
