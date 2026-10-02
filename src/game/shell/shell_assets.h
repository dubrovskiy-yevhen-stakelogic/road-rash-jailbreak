#pragma once
// Front-end ("shell") asset parsers of Road Rash: Jailbreak (USA, SLUS_01053). The authoritative
// C++ readers for:
//   * MDEC pictures and films: DATA\FE\*.STR, *.WVE (chunk chains) and *.TCM (offset archive) -
//     docs\formats\video.md. The entropy code book, the
//     default symbol table, the quantisation tables and the IDCT matrix are all read at run time
//     from the player's own SLUS_010.53 - none of their contents live in this source.
//   * DATA\FE\FEMISC.PSH, the EA "SHPP" shape bank - docs\formats\textures.md section 4.
//   * *.PFN "FNTP" 4bpp bitmap fonts - docs\formats\textures.md section 6.
//   * *.LOC "LOCH"/"LOCL" string pools - docs\formats\frontend.md section 10.
// Every parser is strict: a header invariant the docs list that does not hold throws
// std::runtime_error; nothing returns a partially decoded asset.
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "rrformats/mdec.h"

namespace rr::shell {

// ---------------------------------------------------------------------------------------------
// MDEC
// ---------------------------------------------------------------------------------------------

constexpr int kMdecCodewords = 224;  // 96 short + 128 long codewords (video.md section 3)

// Everything the game's BS decoder (SLUS 0x80020400) and the MDEC it feeds need, as read from the EXE:
//   * the 224 codewords the table builder 0x800202B8 walks (descriptor arrays at 0x800528A0 and
//     0x80052A20),
//   * the symbol table (the EXE default at 0x80052C20, or a VLC0 chunk's own - see WithSymbols),
//   * the two quantisation tables and the IDCT matrix that the library's DecDCTReset uploads with
//     MDEC commands 2 and 3 (the command words 0x40000001 / 0x60000000 sit in front of them at
//     0x8005A24C / 0x8005A2D0; tables at 0x8005A250 (luma, 64 bytes zig-zag order), 0x8005A290
//     (chroma) and 0x8005A2D4 (64 x s16)).
struct MdecCodebook {
    std::array<uint8_t, kMdecCodewords> codeLength{};  // total bits, 2..17
    std::array<uint32_t, kMdecCodewords> codeBits{};   // right-aligned codeword
    std::array<uint16_t, kMdecCodewords> symbols{};    // MDEC halfword per codeword
    std::array<uint8_t, 64> quantLuma{};               // zig-zag order, as uploaded
    std::array<uint8_t, 64> quantChroma{};
    std::array<int16_t, 64> idctMatrix{};              // row = frequency, column = sample
    std::vector<uint8_t> lookup;                       // 1<<17 entries: codeword index + 1, 0 = none

    // Same codewords and MDEC tables, symbols replaced (the payload of a .WVE VLC0 chunk).
    MdecCodebook WithSymbols(std::span<const uint16_t> newSymbols) const;
    // What DecDCTReset uploads (MDEC commands 2 and 3), for the shared MDEC core.
    rr::mdec::Tables MdecTables() const { return rr::mdec::Tables{quantLuma, quantChroma, idctMatrix}; }
};

// Reads the code book from the player's SLUS_010.53 (the whole file, PS-X EXE header included).
// Throws if the file is not a PS-X EXE loaded at 0x80010000, or the tables are not a complete,
// prefix-free code (Kraft sum 4095/4096) with the MDEC command words in place.
MdecCodebook LoadDefaultCodebook(std::span<const uint8_t> slusFile);

// A decoded picture: PS1 BGR555 as the MDEC 15-bit output mode writes it. The game calls
// DecDCTin with mode 2 (RASHCDF 0x80062AB4 loads the mode from 0x800810EC; DecDCTin at SLUS
// 0x8004D90C turns bit 1 into MDEC command bit 25), so bit 15 (STP) is set on every pixel.
// Back end: the MDEC's own arithmetic (rr::mdec, src\rrformats\mdec.h - the shared hardware model:
// dequantisation, IDCT, colour, 15-bit packing), fed the run-level halfwords the game's DctVlc emits.
struct Picture15 {
    int width = 0, height = 0;
    std::vector<uint16_t> px;  // width*height, top row first
};

// One MDEC chunk (tag "MDEC", video.md 1.4) -> picture. `chunk` spans exactly the chunk.
Picture15 DecodeMdecChunk(std::span<const uint8_t> chunk, const MdecCodebook& book);

// Every MDEC chunk of a chunk chain (.STR, or .WVE whose leading VLC0 table replaces the
// default symbols; audio chunks are skipped), in file order.
std::vector<Picture15> DecodeStrFrames(std::span<const uint8_t> file, const MdecCodebook& book);

// Every entry of a .TCM archive (u32 count; {u32 size; u32 offset}[count]; contiguous MDEC chunks).
std::vector<Picture15> DecodeTcmFrames(std::span<const uint8_t> file, const MdecCodebook& book);

// Streaming access for long films: the MDEC chunks of a chain, and the code book the chain uses.
std::vector<std::span<const uint8_t>> ListStrChunks(std::span<const uint8_t> file);
MdecCodebook CodebookForStream(std::span<const uint8_t> file, const MdecCodebook& exeDefault);

namespace detail {
// The decode core, exposed for the verification bench: 8-bit RGB (3 bytes per pixel) before the
// 15-bit packing. Returns the frame accounting the strict checks use.
struct MdecFrameStats {
    int blocks = 0;
    int emittedHalfwords = 0;
    int escapes = 0;
    int paddingBytes = 0;   // bytes after the halfword that holds the DC sentinel
    int paddingValue = -1;  // their common value, 0x100 when they differ, -1 when none
};
MdecFrameStats DecodeMdecChunkRgb(std::span<const uint8_t> chunk, const MdecCodebook& book, int& width,
                                  int& height, std::vector<uint8_t>& rgb);
}  // namespace detail

// ---------------------------------------------------------------------------------------------
// SHPP shape bank (FEMISC.PSH)
// ---------------------------------------------------------------------------------------------

struct Shape {
    char name[5] = {};
    int width = 0, height = 0;
    std::vector<uint16_t> px;  // BGR555 exactly as stored, top row first
};

struct ShapeBank {
    std::vector<Shape> shapes;
    const Shape* Find(const char* fourcc) const;  // nullptr when absent
};

ShapeBank ParseShapeBank(std::span<const uint8_t> file);

// ---------------------------------------------------------------------------------------------
// FNTP bitmap font (*.PFN)
// ---------------------------------------------------------------------------------------------

struct Glyph {
    uint16_t code;
    uint8_t w, h;
    uint16_t x, y;
    uint8_t advance;
    int8_t xoff, yoff;
};

struct Font {
    int lineHeight = 0;
    uint32_t firstCode = 0;
    std::vector<Glyph> glyphs;
    int sheetWidth = 0, sheetHeight = 0;  // pixels
    int sheetVramWords = 0;               // bitmap +0x00: VRAM width in halfwords (64 in all four)
    std::vector<uint8_t> sheet;           // one 4-bit index per pixel, unpacked (low nibble first)
    const Glyph* Find(uint32_t code) const;  // nullptr when the code is outside the table
};

Font ParseFont(std::span<const uint8_t> file);

// ---------------------------------------------------------------------------------------------
// LOCH / LOCL string pool (*.LOC)
// ---------------------------------------------------------------------------------------------

// Strings in id order, raw 8-bit bytes (no terminator).
std::vector<std::string> ParseStringPool(std::span<const uint8_t> file);

// ---------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------

// BGR555 -> RGBA8 packed as r | g<<8 | b<<16 | a<<24 (byte order R,G,B,A in memory). 5->8 bit by
// (c<<3)|(c>>2). Opaque unless px == 0 (PS1 transparent black); the STP bit is ignored.
uint32_t Bgr555ToRgba8(uint16_t px);

// Whole-file reader (throws when the file cannot be read).
std::vector<uint8_t> ReadWholeFile(const std::string& path);

// Verification bench: parses all four fonts, FEMISC.PSH and both string pools, decodes every
// DATA\FE\*.STR / *.WVE / *.TCM and compares against video.md section 6, then locates the decoded
// shapes, font sheets and MDEC pictures in the capture VRAM (stateDir\vram.bin: 1024x512 little-endian
// halfwords extracted one byte late, see the loader) and reports match statistics. Returns a
// human-readable report.
std::string CheckShellAssets(const std::string& discExtractDir, const std::string& stateDir);

}  // namespace rr::shell
