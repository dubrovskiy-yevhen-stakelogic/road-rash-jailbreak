#pragma once
// The PS1 MDEC (the motion decoder at 0x1F801820 / 0x1F801824) - ONE implementation of its arithmetic, shared by
// the product (the sky's column strips, src\game\sky_product.cpp; the menu / intro films,
// src\game\shell\shell_assets.cpp) and the interpreter's MDEC device (src\interp\devices.cpp, the dev oracle).
//
// What the unit does with a decode command (command 1):
//   * run-level halfwords in: a block starts at the first halfword that is not 0xFE00 (padding before a block is
//     skipped); its top 6 bits are the block's quantiser scale q, its low 10 bits (signed) the DC coefficient;
//     every next halfword adds (run + 1) to the coefficient index k and carries the level; the block ends as soon
//     as k >= 63 (an EOB 0xFE00 = run 63 ends it without storing). Blocks go Cr, Cb, Y1, Y2, Y3, Y4 (colour) or
//     Y (monochrome); Cr / Cb use the colour quantisation table, Y the luminance one (command 2, 64 + 64 bytes);
//   * dequantisation keeps 4 fractional bits: DC = (v * qt[0]) << 4, AC = ((v * q * qt[k]) >> 3) << 4, each biased
//     half a step towards zero (-8 for v > 0, +8 for v < 0), q == 0 (DC: q == 0 / AC: q * qt[k] == 0) stores v << 5
//     unscaled; saturated to -0x4000..0x3FFF; stored at the zig-zag position of k;
//   * the IDCT is two 8-point passes with the matrix command 3 uploaded, each entry arithmetically shifted right by
//     3 on upload: pass 1 (sum + 0x4000) >> 15 into 16 bits, pass 2 (sum + 0x4000) >> 15, wrapped to 9 bits and
//     saturated to -128..127;
//   * colour: R = Y + ((359 Cr + 0x80) >> 8), G = Y + (((-88 Cb) & ~0x1F) + ((-183 Cr) & ~7) + 0x80) >> 8,
//     B = Y + ((454 Cb + 0x80) >> 8) - each wrapped to 9 bits and saturated to -128..127, then + 128 unless the
//     command asked for signed output (bit 26); Cr / Cb of a pixel are the 2 x 2 subsampled blocks' sample;
//   * out: 24-bit (R, G, B bytes packed), or 15-bit: each channel min(31, (c8 + 4) >> 3), bit 15 = command bit 25.
// This is the hardware as the PS1 documentation (psx-spx "MDEC") and the accuracy-oriented emulators describe it,
// re-derived here (no third-party code).
//
// RRJB_MDEC=legacy selects the product's earlier arithmetic (a 64-bit IDCT keeping 3 fractional bits, the
// floating colour matrix floored, films-era) - the negative control. RRJB_MDEC=emuold selects the reference
// emulator's default ("old") routines - a MEASUREMENT model only, to name the captures' residue.
#include <array>
#include <cstddef>
#include <cstdint>

namespace rr::mdec {

enum class Model : uint8_t {
    Hardware = 0, // the console (default)
    Legacy = 1,   // the earlier float/IDCT arithmetic (RRJB_MDEC=legacy)
    EmuOld = 2,   // the capture emulator's default routines (RRJB_MDEC=emuold, measurement only)
};
// RRJB_MDEC, read once.
Model ActiveModel();
const char* ModelName(Model m);

// What commands 2 and 3 upload (as the game's DecDCTReset sends them: SLUS 0x8005A24C / 0x8005A2D0).
struct Tables {
    std::array<uint8_t, 64> luma{};   // command 2, first 64 bytes
    std::array<uint8_t, 64> chroma{}; // command 2, next 64 bytes (colour flag set)
    std::array<int16_t, 64> idct{};   // command 3, 64 x s16 as uploaded (row = frequency, column = sample)
};

// A stream of run-level halfwords (the decode command's parameter words, low halfword first).
struct Input {
    const uint16_t* data = nullptr;
    size_t size = 0;
    size_t pos = 0;
    bool Empty() const { return pos >= size; }
    uint16_t Next() { return pos < size ? data[pos++] : static_cast<uint16_t>(0xFE00u); }
};

// One block: skip the padding, read the run-levels, dequantise (into `coef`, the model's precision, zig-zag
// applied). False when the input ends before the block's first halfword.
bool ReadBlock(Input& in, const std::array<uint8_t, 64>& qt, int32_t coef[64], Model m);
// The IDCT of one block, in place: `blk` in the precision ReadBlock left, out the pixel values -128..127
// (Legacy: 3 fractional bits, unclamped - its colour stage wants them).
void Idct(int32_t blk[64], const std::array<int16_t, 64>& matrix, Model m);
// One pixel's colour, 0x00BBGGRR bytes (unsigned output: + 128 applied; signed: two's complement bytes).
uint32_t Colour(int32_t y, int32_t cb, int32_t cr, bool signedOut, Model m);
// A monochrome pixel (depth 4 / 8 bit), the byte.
uint8_t Mono(int32_t y, bool signedOut, Model m);
// 24-bit colour -> the 15-bit output pixel.
uint16_t To15(uint32_t rgb, bool stp, Model m);

// A colour macroblock: six blocks read and transformed, 16 x 16 pixels (row-major, 0x00BBGGRR) into `rgb`.
// False when the input ran out (the unit then waits for more words).
bool DecodeColourMacroblock(Input& in, const Tables& t, bool signedOut, Model m, uint32_t rgb[256]);
// A monochrome 8 x 8 block into `y8` (bytes).
bool DecodeMonoBlock(Input& in, const Tables& t, bool signedOut, Model m, uint8_t y8[64]);

// The whole decode command (command word `cmd`, bits 27..28 depth, 26 signed, 25 STP) over `in`: the output
// words the unit pushes to its out FIFO, appended to `out` (15-bit: 128 words a macroblock, 24-bit 192, 8-bit 16,
// 4-bit 8). Returns the macroblocks / blocks decoded.
size_t DecodeCommand(uint32_t cmd, Input& in, const Tables& t, Model m, uint32_t* out, size_t outCapacityWords,
                     size_t& outWords);

extern const uint8_t kZigZag[64]; // zig-zag index -> raster position

} // namespace rr::mdec
