#include "rrformats/texture.h"

#include <cstring>
#include <stdexcept>
#include <string>

namespace rr {
namespace {

uint16_t ReadU16(std::span<const uint8_t> d, size_t off) {
    if (off + 2 > d.size()) throw std::runtime_error("texture: read past end of container");
    return static_cast<uint16_t>(d[off] | (d[off + 1] << 8));
}

uint32_t ReadU32(std::span<const uint8_t> d, size_t off) {
    if (off + 4 > d.size()) throw std::runtime_error("texture: read past end of container");
    return static_cast<uint32_t>(d[off]) | (static_cast<uint32_t>(d[off + 1]) << 8) |
           (static_cast<uint32_t>(d[off + 2]) << 16) | (static_cast<uint32_t>(d[off + 3]) << 24);
}

bool TagIs(std::span<const uint8_t> d, size_t off, const char* tag) {
    return off + 4 <= d.size() && std::memcmp(d.data() + off, tag, 4) == 0;
}

// Expands packed indices into one byte per pixel. 4bpp is low nibble first, established by decoding
// HAZARD0.TEX both ways: low nibble first gives legible signs, high nibble first gives garbage.
std::vector<uint8_t> Unpack(std::span<const uint8_t> src, int width, int height, int bpp) {
    std::vector<uint8_t> out(static_cast<size_t>(width) * static_cast<size_t>(height));
    if (bpp == 8) {
        if (src.size() < out.size()) throw std::runtime_error("texture: 8bpp payload is short");
        std::memcpy(out.data(), src.data(), out.size());
        return out;
    }
    if (bpp != 4) throw std::runtime_error("texture: unsupported bpp " + std::to_string(bpp));
    const size_t need = out.size() / 2;
    if (src.size() < need) throw std::runtime_error("texture: 4bpp payload is short");
    for (size_t i = 0; i < need; ++i) {
        out[i * 2 + 0] = static_cast<uint8_t>(src[i] & 0x0F);
        out[i * 2 + 1] = static_cast<uint8_t>(src[i] >> 4);
    }
    return out;
}

Image Colourise(const std::vector<uint8_t>& indices, int width, int height, std::span<const uint32_t> palette) {
    Image image;
    image.width = width;
    image.height = height;
    image.rgba.resize(indices.size() * 4);
    for (size_t i = 0; i < indices.size(); ++i) {
        const uint8_t index = indices[i];
        const uint32_t colour = index < palette.size() ? palette[index] : 0u;
        image.rgba[i * 4 + 0] = static_cast<uint8_t>(colour >> 0);
        image.rgba[i * 4 + 1] = static_cast<uint8_t>(colour >> 8);
        image.rgba[i * 4 + 2] = static_cast<uint8_t>(colour >> 16);
        image.rgba[i * 4 + 3] = static_cast<uint8_t>(colour >> 24);
    }
    return image;
}

} // namespace

uint32_t Bgr555ToRgba(uint16_t texel, bool* semiTransparent) {
    if (semiTransparent) *semiTransparent = (texel & 0x8000) != 0;
    if (texel == 0) return 0; // PS1 convention: an all-zero texel is fully transparent
    const uint32_t r = texel & 0x1F;
    const uint32_t g = (texel >> 5) & 0x1F;
    const uint32_t b = (texel >> 10) & 0x1F;
    const uint32_t r8 = (r << 3) | (r >> 2);
    const uint32_t g8 = (g << 3) | (g >> 2);
    const uint32_t b8 = (b << 3) | (b >> 2);
    return r8 | (g8 << 8) | (b8 << 16) | (0xFFu << 24);
}

Image ApplyPalette(const TextureChunk& chunk, std::span<const uint32_t> palette) {
    if (chunk.indices.empty()) return {};
    return Colourise(chunk.indices, chunk.width, chunk.height, palette);
}

std::vector<TextureChunk> ParseTextureContainer(std::span<const uint8_t> data) {
    std::vector<TextureChunk> chunks;
    size_t at = 0;
    while (at + 8 <= data.size()) {
        const uint32_t size = ReadU32(data, at + 4);
        if (size < 8 || at + size > data.size()) break; // end of the chain
        if (!TagIs(data, at, "LECT")) {
            at += size; // CTKP / TSLP / KNBP / RMD3 / DMD3 - not ours
            continue;
        }

        TextureChunk chunk;
        chunk.kind = data[at + 0x0C];
        chunk.bpp = data[at + 0x0D];
        chunk.slot = data[at + 0x0F];
        chunk.id = ReadU16(data, at + 0x10);
        chunk.width = ReadU16(data, at + 0x12);
        chunk.height = static_cast<int>(ReadU32(data, at + 0x14));
        const size_t payload = at + 0x18;

        if (chunk.kind == 1 || chunk.kind == 2 || chunk.kind == 3) {
            // The payload is a complete PS1 TIM: id, flags, then a CLUT block and an image block,
            // each `u32 bnum; u16 dx; u16 dy; u16 w; u16 h;` with `bnum` covering the whole block.
            if (ReadU32(data, payload) != 0x10) throw std::runtime_error("texture: LECT payload is not a TIM");
            const uint32_t flags = ReadU32(data, payload + 4);
            const uint32_t pmode = flags & 7;
            const bool hasClut = (flags & 8) != 0;
            size_t block = payload + 8;

            if (hasClut) {
                const uint32_t bnum = ReadU32(data, block);
                const uint16_t entries = ReadU16(data, block + 8);
                const uint16_t rows = ReadU16(data, block + 10);
                chunk.palette.reserve(static_cast<size_t>(entries) * rows);
                for (uint32_t i = 0; i < static_cast<uint32_t>(entries) * rows; ++i)
                    chunk.palette.push_back(Bgr555ToRgba(ReadU16(data, block + 12 + i * 2)));
                chunk.hasPalette = true;
                block += bnum;
            }

            const uint16_t words = ReadU16(data, block + 8);
            const uint16_t rows = ReadU16(data, block + 10);
            // `w` counts 16-bit VRAM words, so the pixel width depends on the bit depth.
            const int pixelWidth = pmode == 0 ? words * 4 : pmode == 1 ? words * 2 : words;
            const int bitsPerPixel = pmode == 0 ? 4 : pmode == 1 ? 8 : 16;
            if (pixelWidth != chunk.width || rows != chunk.height)
                throw std::runtime_error("texture: TIM block size disagrees with the LECT header");

            const size_t pixels = block + 12;
            if (bitsPerPixel == 16) {
                chunk.decoded.width = pixelWidth;
                chunk.decoded.height = rows;
                chunk.decoded.rgba.resize(static_cast<size_t>(pixelWidth) * rows * 4);
                for (size_t i = 0; i < static_cast<size_t>(pixelWidth) * rows; ++i) {
                    const uint32_t colour = Bgr555ToRgba(ReadU16(data, pixels + i * 2));
                    std::memcpy(chunk.decoded.rgba.data() + i * 4, &colour, 4);
                }
            } else {
                chunk.indices = Unpack(data.subspan(pixels, data.size() - pixels), pixelWidth, rows, bitsPerPixel);
                chunk.decoded = Colourise(chunk.indices, pixelWidth, rows, chunk.palette);
            }
        } else if (chunk.kind == 5 || chunk.kind == 6) {
            // Raw indexed pixels; the palette lives in VRAM, not in the file.
            chunk.indices = Unpack(data.subspan(payload, data.size() - payload), chunk.width, chunk.height, chunk.bpp);
        } else {
            throw std::runtime_error("texture: unknown LECT kind " + std::to_string(chunk.kind));
        }

        chunks.push_back(std::move(chunk));
        at += size;
    }
    return chunks;
}

} // namespace rr
