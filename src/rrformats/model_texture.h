#pragma once
// Which image and which palette a model's primitives sample.
//
// The rule, traced end to end in a frame of the original:
//   * `DOD3+0x1C` is the model's LECT texture id - a field of the .GEO itself, the same in every group.
//   * That id selects a LECT chunk (`RASHCDI 0x8005C054` searches the page table for it).
//   * For a 4bpp atlas the palette is NOT stored separately: it ships inside the image, in the last
//     rows the art never reaches, and `prim.tpage` picks one. The engine computes the VRAM address
//     with a 40-entry loop at `0x800255B8`; collapsed, palette `n` is the 16 halfwords at payload
//     offset `(height-1-(n>>2))*(width/2) + (n&3)*32`.
//   * For an 8bpp sheet the palette is a 128-entry block of the container's `KNBP` bank and
//     `prim.tpage` is 0 throughout.
//   * `prim.clut` bit 7 is a one-bit page selector, not a CLUT id: on the bike it sends the wheels to
//     a different sheet.
#include "rrformats/rmd3.h"
#include "rrformats/texture.h"

#include <cstdint>
#include <span>
#include <vector>

namespace rr {

// An indexed image plus the palettes its primitives may select. Keeping the two apart is what the
// hardware does, and it lets one upload serve every palette.
struct IndexedTexture {
    int width = 0;
    int height = 0;
    int bpp = 0;
    std::vector<uint8_t> indices;   // width*height, one byte per pixel
    std::vector<uint32_t> palettes; // paletteCount * paletteSize, RGBA
    int paletteCount = 0;
    int paletteSize = 0;
    bool Empty() const { return indices.empty(); }
};

// The texture id a model's primitives sample, from `DOD3+0x1C` of its first group.
uint32_t ModelTextureId(const Model& model);

// Builds the indexed texture for a 4bpp atlas (the roadside props): the art rows as indices, and one
// palette per `prim.tpage`, read out of the image's own trailing rows.
IndexedTexture BuildPropAtlas(std::span<const uint8_t> texFile, int maxPalettes = 48);

// The same for the 4bpp LECT chunk `textureId` of a container holding many (`-1`: the first). The
// weapons (model 800, DOD3+0x1C = 49 in BBLEVEL<n>.TEX) are drawn this way: 0x800251E4's CLUT loop
// takes the model key record of the object's +0x4A (rr-race: key 49, CLUT spec 0x0FFE, x 0x3C0) and
// puts palette n at VRAM (960 + 16 (n & 3), 255 - (n >> 2)) - the image's own last rows.
IndexedTexture BuildLectAtlas(std::span<const uint8_t> texFile, int textureId, int maxPalettes = 48);

// Builds the indexed texture for an 8bpp sheet whose palette bank is a `KNBP` block in the same
// container. `paletteIndex` is the **block number inside the `KNBP` payload**, 256 bytes apart -
// NOT the racer skin index `a1` the draw turns into a CLUT id. For the player's bike in the captured
// `rr-race` state `a1 = 29`, which is `KNBP` block 46: the 128 halfwords at `BBLEVEL1.TEX +0x2E14`
// are byte-identical to the VRAM block at (896, 502) that CLUT id `0x7DB8` names, and `0x7DB8` is
// the value `a1 = 29` produces and the value the GPU received.
//
// `bankTag` picks which bank of the container the block is counted in: `KNBP` for the bike, `TSLP`
// for the rider (see the two constants below).
IndexedTexture BuildSheetWithBank(std::span<const uint8_t> texFile, uint16_t textureId, int paletteIndex,
                                  const char* bankTag = "KNBP");

// The `KNBP` block the player's bike and rider are drawn with in the captured `rr-race` state.
//
// Two separate facts, both measured, and they do not compose the way the positional rule suggests:
//   * the object's skin index is `a1 = 29` (`obj 0x801B65D4 + 0x24` bits 12..17 in
//     `work\oracle\state\rr-race\ram.bin`), which the draw turns into CLUT id `0x7DB8`
//     = VRAM (896, 502);
//   * VRAM (896, 502) in that state is byte-identical to `DATA\BBLEVEL1.TEX +0x2E14`, i.e. `KNBP`
//     block 46 - all 256 bytes.
// The positional upload rule (`block k -> VRAM (640 + (k%3)*128, 511 - (47-k)/3)`, verified here
// for 23 of the 48 blocks) puts block 46 at (768, 511), which is `a1 = 1`; block 20, the block the
// rule assigns to `a1 = 29`, is all zero on the disc. So the racer's row is filled at run time with
// a COPY of block 46 by a path we have not traced - the bytes are the same either way, which is why
// a renderer can bind block 46 and reproduce the frame.
constexpr int kPlayerBikeKnbpBlock = 46;

// The rider is a SEPARATE object with a palette of its own: object `0x801BB2EC` (model 150) has
// `a1 = 30`, which the draw turns into CLUT id `0x7D68` = VRAM (640, 501), and those 128
// halfwords are byte-identical to `DATA\BBLEVEL1.TEX +0x3128` - the `TSLP` bank (payload at
// `+0x3028`), block 1 at the same 256-byte stride. So the bike takes its palette from `KNBP` and
// the rider from `TSLP`, which is why binding one palette to both makes the rider come out in the
// bike's colours.
constexpr int kPlayerRiderTslpBlock = 1;

// An 8bpp PS1 `TIM` file (`DATA\RIMA1.TIM` - the wheel rim sheet `prim.clut` bit 7 selects) whose
// palette is taken from a `KNBP` bank in another container, because an 8bpp palette is a property
// of the OBJECT and not of the image. Pass an empty `bank` to use the TIM's
// own CLUT instead.
IndexedTexture BuildTimSheetWithBank(std::span<const uint8_t> tim, std::span<const uint8_t> bank,
                                     int paletteIndex, const char* bankTag = "KNBP");

// ------------------------------------------------------------------ scene-cell pages
// A scene-cell texture page, built from the type-1 / type-2 stream chunks the cell's own header pair
// names. Rule and evidence in docs\formats\scene_cell.md section 12; in short, a chunk IS a VRAM
// rectangle of 64 x 128 halfwords at 4bpp (256 x 128 texels), written from the chunk's own bytes:
//   * a type-2 chunk is one such rectangle and stands alone - that is what band 0 samples;
//   * the two type-1 chunks of one resource id are the top and bottom halves of a 256 x 256 page -
//     that is what band 1 samples.
// The palettes travel with the image: the LAST chunk's rows 96..127, halfword columns 48..63, are 32
// palettes of 16 BGR555 entries, one per primitive `pal` value. The chunk's own 32-byte header is
// never uploaded, so texels 0..63 of row 0 are transparent on the console and are forced to index 0
// here.
//
// `second` is empty for a type-2 page. Both spans must be whole 0x4000 chunks.
IndexedTexture BuildCellTexturePage(std::span<const uint8_t> first, std::span<const uint8_t> second = {});

// The page a cell primitive with `texRef == 0x7800` samples: `DATA\G_OBJ01.GTP`, 16384 bytes with no
// header at all, uploaded verbatim to VRAM (896, 0) as 64 x 128 halfwords = 256 x 128 texels at 4bpp
// (docs\formats\textures.md 5, reproduced byte-for-byte against `rr-race`). Its 14 palettes ship in
// its OWN FIRST FOUR ROWS, four per row of 16 halfwords, which is the opposite end of the page from
// the stream chunks' - the fix-up pass reads its position from a different config group.
// Palette `pal` is the 16 halfwords at byte offset `(pal / 4) * 128 + (pal % 4) * 32`.
IndexedTexture BuildRuntimeObjectPage(std::span<const uint8_t> gtp);

// Rows 30 and 31 of a page's palette block do not hold palettes: the loader parks the uploaded
// chunk's own 32-byte header there (proven against `work\oracle\state\rr-race\vram.bin`). No shipped
// primitive selects them - `pal` reaches 29 at most over both streams - so they are reported rather
// than used.
constexpr int kCellPaletteCount = 32;
constexpr int kCellPaletteUsable = 30;

} // namespace rr
