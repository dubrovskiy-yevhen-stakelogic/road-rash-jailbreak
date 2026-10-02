#include "rrformats/model_texture.h"

#include <cstring>
#include <stdexcept>
#include <string>

namespace rr {
namespace {

uint16_t ReadU16(std::span<const uint8_t> d, size_t off) {
    if (off + 2 > d.size()) throw std::runtime_error("model_texture: read past end of container");
    return static_cast<uint16_t>(d[off] | (d[off + 1] << 8));
}

uint32_t ReadU32(std::span<const uint8_t> d, size_t off) {
    if (off + 4 > d.size()) throw std::runtime_error("model_texture: read past end of container");
    return static_cast<uint32_t>(d[off]) | (static_cast<uint32_t>(d[off + 1]) << 8) |
           (static_cast<uint32_t>(d[off + 2]) << 16) | (static_cast<uint32_t>(d[off + 3]) << 24);
}

bool TagIs(std::span<const uint8_t> d, size_t off, const char* tag) {
    return off + 4 <= d.size() && std::memcmp(d.data() + off, tag, 4) == 0;
}

// Finds a chunk of the generic `tag + u32 size` chain. `wantId` < 0 means "the first one".
bool FindChunk(std::span<const uint8_t> file, const char* tag, int wantId, size_t& offsetOut, size_t& sizeOut) {
    size_t at = 0;
    while (at + 8 <= file.size()) {
        const uint32_t size = ReadU32(file, at + 4);
        if (size < 8 || at + size > file.size()) break;
        if (TagIs(file, at, tag) && (wantId < 0 || ReadU16(file, at + 0x10) == static_cast<uint16_t>(wantId))) {
            offsetOut = at;
            sizeOut = size;
            return true;
        }
        at += size;
    }
    return false;
}

} // namespace

uint32_t ModelTextureId(const Model& model) {
    return model.groups.empty() ? 0u : model.groups.front().slot;
}

IndexedTexture BuildPropAtlas(std::span<const uint8_t> texFile, int maxPalettes) {
    return BuildLectAtlas(texFile, -1, maxPalettes);
}

IndexedTexture BuildLectAtlas(std::span<const uint8_t> texFile, int textureId, int maxPalettes) {
    size_t chunkAt = 0, chunkSize = 0;
    if (!FindChunk(texFile, "LECT", textureId, chunkAt, chunkSize))
        throw std::runtime_error("model_texture: no LECT chunk " + std::to_string(textureId) + " in the container");

    const uint8_t bpp = texFile[chunkAt + 0x0D];
    if (bpp != 4) throw std::runtime_error("model_texture: prop atlas is not 4bpp");
    IndexedTexture out;
    out.bpp = 4;
    out.width = ReadU16(texFile, chunkAt + 0x12);
    out.height = static_cast<int>(ReadU32(texFile, chunkAt + 0x14));
    const size_t payload = chunkAt + 0x18;
    const size_t rowBytes = static_cast<size_t>(out.width) / 2;

    // Art: low nibble first.
    out.indices.resize(static_cast<size_t>(out.width) * out.height);
    for (int y = 0; y < out.height; ++y)
        for (int x = 0; x < out.width; ++x) {
            const uint8_t packed = texFile[payload + static_cast<size_t>(y) * rowBytes + static_cast<size_t>(x) / 2];
            out.indices[static_cast<size_t>(y) * out.width + static_cast<size_t>(x)] =
                (x & 1) ? static_cast<uint8_t>(packed >> 4) : static_cast<uint8_t>(packed & 0x0F);
        }

    // Palettes: 16 halfwords at (height-1-(n>>2))*rowBytes + (n&3)*32, inside the image's own trailing
    // rows. `prim.tpage` is the index.
    out.paletteSize = 16;
    out.paletteCount = maxPalettes;
    out.palettes.resize(static_cast<size_t>(out.paletteCount) * out.paletteSize);
    for (int n = 0; n < out.paletteCount; ++n) {
        const size_t row = static_cast<size_t>(out.height - 1 - (n >> 2));
        const size_t at = payload + row * rowBytes + static_cast<size_t>(n & 3) * 32;
        for (int i = 0; i < 16; ++i)
            out.palettes[static_cast<size_t>(n) * 16 + static_cast<size_t>(i)] =
                Bgr555ToRgba(ReadU16(texFile, at + static_cast<size_t>(i) * 2));
    }
    return out;
}

IndexedTexture BuildSheetWithBank(std::span<const uint8_t> texFile, uint16_t textureId, int paletteIndex,
                                  const char* bankTag) {
    size_t chunkAt = 0, chunkSize = 0;
    if (!FindChunk(texFile, "LECT", static_cast<int>(textureId), chunkAt, chunkSize))
        throw std::runtime_error("model_texture: no LECT chunk with id " + std::to_string(textureId));

    // Kinds 1/2/3 wrap a complete TIM; the indices live in its image block.
    const size_t tim = chunkAt + 0x18;
    if (ReadU32(texFile, tim) != 0x10) throw std::runtime_error("model_texture: LECT payload is not a TIM");
    const uint32_t flags = ReadU32(texFile, tim + 4);
    size_t block = tim + 8;
    if (flags & 8) block += ReadU32(texFile, block); // skip the TIM's own CLUT block
    const uint16_t words = ReadU16(texFile, block + 8);
    const uint16_t rows = ReadU16(texFile, block + 10);

    IndexedTexture out;
    out.bpp = 8;
    out.width = words * 2; // 8bpp: two pixels per VRAM halfword
    out.height = rows;
    out.indices.assign(texFile.begin() + static_cast<ptrdiff_t>(block + 12),
                       texFile.begin() + static_cast<ptrdiff_t>(block + 12) +
                           static_cast<ptrdiff_t>(out.width) * out.height);

    // The palette comes from the container's KNBP bank: 48 blocks of 128 entries, payload at +0x14.
    size_t bankAt = 0, bankSize = 0;
    out.paletteSize = 128;
    out.paletteCount = 1;
    out.palettes.resize(128);
    if (FindChunk(texFile, bankTag, -1, bankAt, bankSize)) {
        const size_t block0 = bankAt + 0x14;
        const size_t at = block0 + static_cast<size_t>(paletteIndex) * 256;
        for (int i = 0; i < 128; ++i)
            out.palettes[static_cast<size_t>(i)] = Bgr555ToRgba(ReadU16(texFile, at + static_cast<size_t>(i) * 2));
    } else if (flags & 8) {
        // No bank in this container: fall back to the TIM's own CLUT, which is what a standalone
        // sheet carries.
        const size_t clut = tim + 8 + 12;
        for (int i = 0; i < 128; ++i)
            out.palettes[static_cast<size_t>(i)] = Bgr555ToRgba(ReadU16(texFile, clut + static_cast<size_t>(i) * 2));
    }
    return out;
}

IndexedTexture BuildTimSheetWithBank(std::span<const uint8_t> tim, std::span<const uint8_t> bank,
                                     int paletteIndex, const char* bankTag) {
    if (ReadU32(tim, 0) != 0x10) throw std::runtime_error("model_texture: not a TIM");
    const uint32_t flags = ReadU32(tim, 4);
    if ((flags & 7) != 1) throw std::runtime_error("model_texture: TIM is not 8bpp");
    size_t block = 8;
    const size_t clut = (flags & 8) ? block + 12 : 0;
    if (flags & 8) block += ReadU32(tim, block); // skip the TIM's own CLUT block
    const uint16_t words = ReadU16(tim, block + 8);
    const uint16_t rows = ReadU16(tim, block + 10);

    IndexedTexture out;
    out.bpp = 8;
    out.width = words * 2; // 8bpp: two pixels per VRAM halfword
    out.height = rows;
    const size_t pixels = static_cast<size_t>(out.width) * out.height;
    if (block + 12 + pixels > tim.size()) throw std::runtime_error("model_texture: TIM image block is short");
    out.indices.assign(tim.begin() + static_cast<ptrdiff_t>(block + 12),
                       tim.begin() + static_cast<ptrdiff_t>(block + 12 + pixels));

    out.paletteSize = 128;
    out.paletteCount = 1;
    out.palettes.assign(128, 0);
    size_t bankAt = 0, bankSize = 0;
    if (!bank.empty() && FindChunk(bank, bankTag, -1, bankAt, bankSize)) {
        const size_t at = bankAt + 0x14 + static_cast<size_t>(paletteIndex) * 256;
        for (int i = 0; i < 128; ++i)
            out.palettes[static_cast<size_t>(i)] = Bgr555ToRgba(ReadU16(bank, at + static_cast<size_t>(i) * 2));
    } else if (clut != 0) {
        for (int i = 0; i < 128; ++i)
            out.palettes[static_cast<size_t>(i)] = Bgr555ToRgba(ReadU16(tim, clut + static_cast<size_t>(i) * 2));
    }
    return out;
}

IndexedTexture BuildRuntimeObjectPage(std::span<const uint8_t> gtp) {
    constexpr size_t kRowBytes = 128;
    constexpr int kRows = 128;
    constexpr int kWidth = 256;
    if (gtp.size() < kRowBytes * kRows) throw std::runtime_error("model_texture: G_OBJ01.GTP is short");

    IndexedTexture out;
    out.bpp = 4;
    out.width = kWidth;
    out.height = kRows;
    out.indices.resize(static_cast<size_t>(kWidth) * kRows);
    for (int y = 0; y < kRows; ++y)
        for (int x = 0; x < kWidth; ++x) {
            const uint8_t packed = gtp[static_cast<size_t>(y) * kRowBytes + static_cast<size_t>(x) / 2];
            out.indices[static_cast<size_t>(y) * kWidth + static_cast<size_t>(x)] =
                (x & 1) ? static_cast<uint8_t>(packed >> 4) : static_cast<uint8_t>(packed & 0x0F);
        }

    // 16 slots, of which the shipped data fills 14 - the highest `pal` any cell primitive carries on
    // this page is 13, over all 102 stream files.
    out.paletteSize = 16;
    out.paletteCount = 16;
    out.palettes.resize(static_cast<size_t>(out.paletteCount) * out.paletteSize);
    for (int n = 0; n < out.paletteCount; ++n) {
        const size_t at = static_cast<size_t>(n / 4) * kRowBytes + static_cast<size_t>(n % 4) * 32;
        for (int i = 0; i < 16; ++i)
            out.palettes[static_cast<size_t>(n) * 16 + static_cast<size_t>(i)] =
                Bgr555ToRgba(ReadU16(gtp, at + static_cast<size_t>(i) * 2));
    }
    return out;
}

IndexedTexture BuildCellTexturePage(std::span<const uint8_t> first, std::span<const uint8_t> second) {
    // A chunk is a 64 x 128 halfword VRAM rectangle: 128 bytes per row, 128 rows, 4bpp.
    constexpr size_t kRowBytes = 128;
    constexpr int kRows = 128;
    constexpr int kWidth = 256; // texels per row at 4bpp
    if (first.size() < kRowBytes * kRows)
        throw std::runtime_error("model_texture: cell texture chunk is short");
    if (!second.empty() && second.size() < kRowBytes * kRows)
        throw std::runtime_error("model_texture: second cell texture chunk is short");

    IndexedTexture out;
    out.bpp = 4;
    out.width = kWidth;
    out.height = second.empty() ? kRows : kRows * 2;
    out.indices.resize(static_cast<size_t>(out.width) * out.height);

    const std::span<const uint8_t> halves[2] = {first, second};
    const int halfCount = second.empty() ? 1 : 2;
    for (int h = 0; h < halfCount; ++h)
        for (int y = 0; y < kRows; ++y)
            for (int x = 0; x < kWidth; ++x) {
                const uint8_t packed = halves[h][static_cast<size_t>(y) * kRowBytes + static_cast<size_t>(x) / 2];
                const size_t at = (static_cast<size_t>(h) * kRows + static_cast<size_t>(y)) * kWidth +
                                  static_cast<size_t>(x);
                out.indices[at] = (x & 1) ? static_cast<uint8_t>(packed >> 4) : static_cast<uint8_t>(packed & 0x0F);
            }

    // The chunk's own 32-byte header is not uploaded: the console leaves those 16 halfwords - texels
    // 0..63 of row 0 - untouched, and they read as zero. Index 0 is the transparent entry, so this is
    // what the hardware shows, not a patch over a decoding problem. A two-chunk type-1 page has one
    // such header per chunk, at page rows 0 AND 128 (scene_cell.md 12.4: the VRAM comparison finds
    // exactly rows 0, 128, 254 and 255 differing on a 64x256 rectangle).
    for (int h = 0; h < halfCount; ++h)
        for (int x = 0; x < 64; ++x)
            out.indices[static_cast<size_t>(h) * kRows * kWidth + static_cast<size_t>(x)] = 0;

    // Palettes: the LAST chunk's rows 96..127, halfword columns 48..63. For a type-2 page that is
    // rows 96..127 of the page itself; for a type-1 page it is page rows 224..255, which is where the
    // resolver's `clutY = 224 + pal` lands.
    const std::span<const uint8_t>& paletteChunk = second.empty() ? first : second;
    out.paletteSize = 16;
    out.paletteCount = kCellPaletteCount;
    out.palettes.resize(static_cast<size_t>(out.paletteCount) * out.paletteSize);
    for (int n = 0; n < out.paletteCount; ++n) {
        const size_t at = static_cast<size_t>(kRows - kCellPaletteCount + n) * kRowBytes + 96;
        for (int i = 0; i < 16; ++i)
            out.palettes[static_cast<size_t>(n) * 16 + static_cast<size_t>(i)] =
                Bgr555ToRgba(ReadU16(paletteChunk, at + static_cast<size_t>(i) * 2));
    }
    return out;
}

} // namespace rr
