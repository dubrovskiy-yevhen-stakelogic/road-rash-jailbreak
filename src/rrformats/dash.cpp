#include "rrformats/dash.h"

#include <charconv>
#include <stdexcept>

namespace rr {
namespace {

std::vector<std::string> SplitFields(std::string_view line) {
    std::vector<std::string> fields;
    size_t at = 0;
    while (at <= line.size()) {
        const size_t comma = line.find(',', at);
        const size_t end = comma == std::string_view::npos ? line.size() : comma;
        std::string_view field = line.substr(at, end - at);
        while (!field.empty() && (field.back() == '\r' || field.back() == ' ')) field.remove_suffix(1);
        while (!field.empty() && field.front() == ' ') field.remove_prefix(1);
        fields.emplace_back(field);
        if (comma == std::string_view::npos) break;
        at = comma + 1;
    }
    return fields;
}

int FieldToInt(const std::vector<std::string>& fields, size_t index) {
    if (index >= fields.size() || fields[index].empty()) return 0;
    int value = 0;
    const std::string& s = fields[index];
    const std::from_chars_result r = std::from_chars(s.data(), s.data() + s.size(), value);
    return r.ec == std::errc() ? value : 0;
}

} // namespace

DashLayout ParseDashCsv(std::string_view text) {
    DashLayout layout;
    size_t at = 0;
    int remainingTextures = -1, remainingArt = 0, remainingItems = 0;

    while (at <= text.size()) {
        const size_t newline = text.find('\n', at);
        std::string_view line = text.substr(at, (newline == std::string_view::npos ? text.size() : newline) - at);
        at = (newline == std::string_view::npos) ? text.size() + 1 : newline + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        if (line.empty()) continue;

        const std::vector<std::string> fields = SplitFields(line);
        if (fields.empty()) continue;

        // Before the counts line everything is a named setting; after it the rows are positional.
        if (remainingTextures < 0) {
            if (fields[0] == "ClutTable") {
                layout.clutTableX = FieldToInt(fields, 1);
                layout.clutTableY = FieldToInt(fields, 2);
            } else if (fields[0] == "TexArtDashCounts") {
                layout.textureCount = FieldToInt(fields, 1);
                layout.artCount = FieldToInt(fields, 2);
                layout.itemCount = FieldToInt(fields, 3);
                remainingTextures = layout.textureCount;
                remainingArt = layout.artCount;
                remainingItems = layout.itemCount;
            }
            continue;
        }

        if (remainingTextures > 0) {
            DashTexture texture;
            texture.name = fields[0];
            texture.vramX = FieldToInt(fields, 1);
            texture.vramY = FieldToInt(fields, 2);
            layout.textures.push_back(std::move(texture));
            --remainingTextures;
        } else if (remainingArt > 0) {
            DashArt art;
            art.name = fields[0];
            art.x = FieldToInt(fields, 1);
            art.y = FieldToInt(fields, 2);
            art.width = FieldToInt(fields, 3);
            art.height = FieldToInt(fields, 4);
            art.textureIndex = FieldToInt(fields, 5);
            layout.art.push_back(std::move(art));
            --remainingArt;
        } else if (remainingItems > 0) {
            DashItem item;
            item.name = fields[0];
            item.screenX = FieldToInt(fields, 1);
            item.screenY = FieldToInt(fields, 2);
            item.artIndex = FieldToInt(fields, 3);
            layout.items.push_back(std::move(item));
            --remainingItems;
        }
    }
    return layout;
}

Image DecodeDashPage(std::span<const uint8_t> page, int clutSlot) {
    if (page.size() < kDashPageSize) throw std::runtime_error("dash: page is not 32768 bytes");

    // CLUT slot n: row 150 + n/4, halfword block n%4.
    const size_t clutOffset = static_cast<size_t>(kDashClutRow) * kDashRowBytes +
                              static_cast<size_t>(clutSlot % 4) * 32 +
                              static_cast<size_t>(clutSlot / 4) * kDashRowBytes;
    if (clutOffset + 32 > page.size()) throw std::runtime_error("dash: CLUT slot is outside the page");
    std::vector<uint32_t> palette(16);
    for (size_t i = 0; i < 16; ++i) {
        const uint16_t texel = static_cast<uint16_t>(page[clutOffset + i * 2] | (page[clutOffset + i * 2 + 1] << 8));
        palette[i] = Bgr555ToRgba(texel);
    }

    // Art rows only; the rest of the page is filler and the CLUT table.
    constexpr int kArtRows = 145;
    Image image;
    image.width = kDashPageWidth;
    image.height = kArtRows;
    image.rgba.resize(static_cast<size_t>(kDashPageWidth) * kArtRows * 4);
    for (int y = 0; y < kArtRows; ++y)
        for (int x = 0; x < kDashPageWidth; ++x) {
            const uint8_t packed = page[static_cast<size_t>(y) * kDashRowBytes + static_cast<size_t>(x) / 2];
            const uint8_t index = (x & 1) ? static_cast<uint8_t>(packed >> 4) : static_cast<uint8_t>(packed & 0x0F);
            const uint32_t colour = palette[index];
            const size_t at = (static_cast<size_t>(y) * kDashPageWidth + static_cast<size_t>(x)) * 4;
            image.rgba[at + 0] = static_cast<uint8_t>(colour >> 0);
            image.rgba[at + 1] = static_cast<uint8_t>(colour >> 8);
            image.rgba[at + 2] = static_cast<uint8_t>(colour >> 16);
            image.rgba[at + 3] = static_cast<uint8_t>(colour >> 24);
        }
    return image;
}

std::vector<Image> DecodeDashTextures(std::span<const uint8_t> page, const DashLayout& layout) {
    std::vector<Image> images;
    images.reserve(layout.textures.size());
    for (size_t i = 0; i < layout.textures.size(); ++i)
        images.push_back(DecodeDashPage(page, static_cast<int>(i)));
    return images;
}

} // namespace rr
