#pragma once
// The HUD page: `DATA\DASH?P.TEX` plus the authoring layout that ships beside it as `DASH?P.CSV`.
//
// The .TEX is a headerless dump of a VRAM rectangle, 64 halfwords wide by 256 rows: 4bpp art in rows
// 0..144, the CLUT table at row 150 (exactly where the CSV's first line says it is), filler elsewhere.
// Established in docs\formats\textures.md section 3.
#include "rrformats/texture.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rr {

constexpr size_t kDashPageSize = 32768;
constexpr int kDashPageWidth = 256;  // pixels at 4bpp
constexpr int kDashPageRows = 256;
constexpr int kDashRowBytes = 128;
constexpr int kDashClutRow = 150;

// One entry of the CSV's texture list: a sub-image of the page with its own CLUT slot.
struct DashTexture {
    std::string name;
    int vramX = 0;
    int vramY = 0;
};

// One drawn HUD element: a rectangle of a texture, placed on screen.
struct DashArt {
    std::string name;
    int x = 0, y = 0, width = 0, height = 0;
    int textureIndex = 0;
};

struct DashItem {
    std::string name;
    int screenX = 0, screenY = 0;
    int artIndex = 0;
};

struct DashLayout {
    int clutTableX = 0, clutTableY = kDashClutRow;
    int textureCount = 0, artCount = 0, itemCount = 0;
    std::vector<DashTexture> textures;
    std::vector<DashArt> art;
    std::vector<DashItem> items;
};

DashLayout ParseDashCsv(std::string_view text);

// The whole page decoded with one CLUT slot. Slot n lives at row 150 + n/4, halfword block n%4 -
// byte offset `150*128 + (n%4)*32 + (n/4)*128`.
Image DecodeDashPage(std::span<const uint8_t> page, int clutSlot);

// The page decoded per texture: image `i` uses CLUT slot `i`, which is the rule the composite in
// docs\formats\textures.md section 3.1 confirms.
std::vector<Image> DecodeDashTextures(std::span<const uint8_t> page, const DashLayout& layout);

} // namespace rr
