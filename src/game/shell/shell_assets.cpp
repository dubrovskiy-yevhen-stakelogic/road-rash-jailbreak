#include "game/shell/shell_assets.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace rr::shell {

namespace {

[[noreturn]] void Fail(const std::string& what) { throw std::runtime_error("shell_assets: " + what); }

std::string Hex(uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "0x%llX", static_cast<unsigned long long>(v));
    return b;
}

void Need(std::span<const uint8_t> d, size_t off, size_t n, const char* what) {
    if (off > d.size() || n > d.size() - off)
        Fail(std::string(what) + ": read of " + std::to_string(n) + " bytes at " + Hex(off) + " is past the end (" +
             std::to_string(d.size()) + " bytes)");
}

uint16_t U16le(std::span<const uint8_t> d, size_t off) {
    Need(d, off, 2, "u16");
    return static_cast<uint16_t>(d[off] | (d[off + 1] << 8));
}
uint32_t U32le(std::span<const uint8_t> d, size_t off) {
    Need(d, off, 4, "u32");
    return static_cast<uint32_t>(d[off]) | (static_cast<uint32_t>(d[off + 1]) << 8) |
           (static_cast<uint32_t>(d[off + 2]) << 16) | (static_cast<uint32_t>(d[off + 3]) << 24);
}
uint16_t U16be(std::span<const uint8_t> d, size_t off) {
    Need(d, off, 2, "u16be");
    return static_cast<uint16_t>((d[off] << 8) | d[off + 1]);
}
uint32_t U32be(std::span<const uint8_t> d, size_t off) {
    Need(d, off, 4, "u32be");
    return (static_cast<uint32_t>(d[off]) << 24) | (static_cast<uint32_t>(d[off + 1]) << 16) |
           (static_cast<uint32_t>(d[off + 2]) << 8) | static_cast<uint32_t>(d[off + 3]);
}
bool TagIs(std::span<const uint8_t> d, size_t off, const char* tag) {
    return off + 4 <= d.size() && std::memcmp(d.data() + off, tag, 4) == 0;
}

// ---------------------------------------------------------------------------------------------
// MDEC constants (video.md sections 1-4; SLUS_010.53 SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1)
// ---------------------------------------------------------------------------------------------

constexpr uint32_t kExeLoad = 0x80010000u;
constexpr uint32_t kExeHeader = 0x800u;
constexpr uint32_t kVaDescShort = 0x800528A0u;   // 96 x {u8 len; u8 pad; u16 code<<(16-len)}
constexpr uint32_t kVaDescLong = 0x80052A20u;    // 128 x same, len counted after 8 zero bits
constexpr uint32_t kVaDefaultSyms = 0x80052C20u; // 224 x u16
constexpr uint32_t kVaQuantCmd = 0x8005A24Cu;    // MDEC command 2 (0x40000001) + 2 x 64 bytes
constexpr uint32_t kVaIdctCmd = 0x8005A2D0u;     // MDEC command 3 (0x60000000) + 64 x s16
constexpr int kShortCodes = 96;
constexpr int kLongCodes = 128;
constexpr int kLookupBits = 17;
constexpr int kEscapeIndex = 23;                 // video.md 3: the slot of 0x7C1F in every table

constexpr uint16_t kEob = 0xFE00;
constexpr uint16_t kEscape = 0x7C1F;
constexpr uint32_t kDcEnd = 0x1FF;
constexpr uint16_t kBsMagic = 0x3800;

size_t ExeOffset(uint32_t va) { return static_cast<size_t>(va - kExeLoad + kExeHeader); }

void CheckSymbols(std::span<const uint16_t> syms) {
    if (syms.size() != kMdecCodewords)
        Fail("symbol table has " + std::to_string(syms.size()) + " entries, expected 224");
    if (syms[kEscapeIndex] != kEscape) Fail("symbol table: slot 23 is not the escape sentinel 0x7C1F");
    if (std::find(syms.begin(), syms.end(), kEob) == syms.end()) Fail("symbol table has no EOB (0xFE00)");
}

// Bits of the entropy-coded stream: u16 little-endian words, each consumed MSB first.
class BitReader {
public:
    explicit BitReader(std::span<const uint8_t> bs) : bits_(static_cast<uint64_t>(bs.size() / 2) * 16) {
        be_.resize(bs.size() / 2 * 2 + 8, 0);
        for (size_t i = 0; i + 1 < bs.size(); i += 2) {
            be_[i] = bs[i + 1];
            be_[i + 1] = bs[i];
        }
    }
    uint32_t Peek(int n) const {
        const size_t byte = static_cast<size_t>(pos_ >> 3);
        const uint32_t w = (static_cast<uint32_t>(be_[byte]) << 24) | (static_cast<uint32_t>(be_[byte + 1]) << 16) |
                           (static_cast<uint32_t>(be_[byte + 2]) << 8) | be_[byte + 3];
        return (w << (pos_ & 7)) >> (32 - n);
    }
    void Skip(int n) {
        pos_ += static_cast<uint64_t>(n);
        if (pos_ > bits_) Fail("MDEC bitstream: read past the end of the chunk");
    }
    uint32_t Read(int n) {
        const uint32_t v = Peek(n);
        Skip(n);
        return v;
    }
    uint64_t Pos() const { return pos_; }
    uint64_t Size() const { return bits_; }

private:
    std::vector<uint8_t> be_;
    uint64_t bits_ = 0;
    uint64_t pos_ = 0;
};

// The IDCT, the colour stage and the 15-bit packing are the MDEC's own: rr::mdec (src\rrformats\mdec.h), the one
// implementation the sky's strips and the interpreter's MDEC device use too.

Picture15 PackPicture(int w, int h, const std::vector<uint8_t>& rgb) {
    Picture15 p;
    p.width = w;
    p.height = h;
    p.px.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
    // The MDEC's 15-bit output mode (command bit 25 set by DecDCTin mode 2: STP on every pixel).
    const rr::mdec::Model m = rr::mdec::ActiveModel();
    for (size_t i = 0; i < p.px.size(); ++i)
        p.px[i] = rr::mdec::To15(static_cast<uint32_t>(rgb[i * 3]) | (static_cast<uint32_t>(rgb[i * 3 + 1]) << 8) |
                                     (static_cast<uint32_t>(rgb[i * 3 + 2]) << 16),
                                 true, m);
    return p;
}

bool IsTcm(std::span<const uint8_t> d) {
    if (d.size() < 12 || TagIs(d, 0, "MDEC") || TagIs(d, 0, "VLC0")) return false;
    const uint32_t count = U32le(d, 0);
    if (count < 1 || count > 4096 || 4 + 8ull * count > d.size()) return false;
    return U32le(d, 8) == 4 + 8 * count && TagIs(d, 4 + 8ull * count, "MDEC");
}

}  // namespace

// =============================================================================================
// MDEC code book
// =============================================================================================

MdecCodebook MdecCodebook::WithSymbols(std::span<const uint16_t> newSymbols) const {
    CheckSymbols(newSymbols);
    MdecCodebook b = *this;
    std::copy(newSymbols.begin(), newSymbols.end(), b.symbols.begin());
    return b;
}

MdecCodebook LoadDefaultCodebook(std::span<const uint8_t> exe) {
    if (exe.size() < kExeHeader || std::memcmp(exe.data(), "PS-X EXE", 8) != 0) Fail("SLUS: not a PS-X EXE");
    if (U32le(exe, 0x18) != kExeLoad) Fail("SLUS: load address is " + Hex(U32le(exe, 0x18)) + ", expected 0x80010000");
    const uint32_t textSize = U32le(exe, 0x1C);
    if (kExeHeader + static_cast<size_t>(textSize) > exe.size()) Fail("SLUS: text size exceeds the file");
    Need(exe, ExeOffset(kVaIdctCmd), 4 + 128, "SLUS MDEC tables");

    MdecCodebook b;
    b.lookup.assign(size_t{1} << kLookupBits, 0);
    uint64_t kraft = 0;
    for (int i = 0; i < kMdecCodewords; ++i) {
        const bool isLong = i >= kShortCodes;
        const size_t off = isLong ? ExeOffset(kVaDescLong) + 4 * static_cast<size_t>(i - kShortCodes)
                                  : ExeOffset(kVaDescShort) + 4 * static_cast<size_t>(i);
        const int len = exe[off];
        const uint16_t code16 = U16le(exe, off + 2);
        if (exe[off + 1] != 0) Fail("SLUS VLC descriptor " + std::to_string(i) + ": pad byte is not 0");
        if (isLong ? (len < 6 || len > 9) : (len < 2 || len > 13))
            Fail("SLUS VLC descriptor " + std::to_string(i) + ": length " + std::to_string(len) + " out of range");
        if ((code16 & ((1u << (16 - len)) - 1)) != 0)
            Fail("SLUS VLC descriptor " + std::to_string(i) + ": code has bits below its length");
        const int total = len + (isLong ? 8 : 0);
        const uint32_t code = static_cast<uint32_t>(code16) >> (16 - len);  // long codes: 8 leading zeros implied
        const uint32_t base = code << (kLookupBits - total);
        // The game routes on "top 8 bits all zero" -> long table; a short code must not start so.
        if (!isLong && (base >> (kLookupBits - 8)) == 0)
            Fail("SLUS VLC descriptor " + std::to_string(i) + ": short code starts with 8 zero bits");
        const uint32_t span = 1u << (kLookupBits - total);
        for (uint32_t k = 0; k < span; ++k) {
            if (b.lookup[base + k] != 0) Fail("SLUS VLC codewords overlap (not prefix-free)");
            b.lookup[base + k] = static_cast<uint8_t>(i + 1);
        }
        kraft += span;
        b.codeLength[static_cast<size_t>(i)] = static_cast<uint8_t>(total);
        b.codeBits[static_cast<size_t>(i)] = code;
    }
    // Kraft sum 4095/4096: only the all-zeros 12-bit prefix is unused (video.md 3).
    if (kraft != (1u << kLookupBits) - (1u << (kLookupBits - 12))) Fail("SLUS VLC code space is not 4095/4096 full");

    std::array<uint16_t, kMdecCodewords> syms{};
    for (int i = 0; i < kMdecCodewords; ++i)
        syms[static_cast<size_t>(i)] = U16le(exe, ExeOffset(kVaDefaultSyms) + 2 * static_cast<size_t>(i));
    CheckSymbols(syms);
    b.symbols = syms;

    if (U32le(exe, ExeOffset(kVaQuantCmd)) != 0x40000001u) Fail("SLUS: MDEC set-quant command word not found");
    if (U32le(exe, ExeOffset(kVaIdctCmd)) != 0x60000000u) Fail("SLUS: MDEC set-IDCT command word not found");
    for (size_t i = 0; i < 64; ++i) {
        b.quantLuma[i] = exe[ExeOffset(kVaQuantCmd) + 4 + i];
        b.quantChroma[i] = exe[ExeOffset(kVaQuantCmd) + 4 + 64 + i];
        b.idctMatrix[i] = static_cast<int16_t>(U16le(exe, ExeOffset(kVaIdctCmd) + 4 + 2 * i));
        if (b.quantLuma[i] == 0 || b.quantChroma[i] == 0) Fail("SLUS: zero entry in an MDEC quant table");
    }
    return b;
}

// =============================================================================================
// MDEC chunk decode
// =============================================================================================

namespace detail {

MdecFrameStats DecodeMdecChunkRgb(std::span<const uint8_t> c, const MdecCodebook& book, int& width, int& height,
                                  std::vector<uint8_t>& rgb) {
    if (book.lookup.size() != (size_t{1} << kLookupBits)) Fail("MDEC: code book not loaded");
    if (!TagIs(c, 0, "MDEC")) Fail("MDEC chunk: bad tag");
    if (c.size() < 0x18) Fail("MDEC chunk: shorter than its header");
    if (U32be(c, 4) != c.size()) Fail("MDEC chunk: size field " + std::to_string(U32be(c, 4)) + " != span " +
                                      std::to_string(c.size()));
    const int w = U16be(c, 8), h = U16be(c, 10);
    const uint16_t nwords = U16le(c, 0x10), magic = U16le(c, 0x12), qscale = U16le(c, 0x14), version = U16le(c, 0x16);
    if (w <= 0 || h <= 0 || w > 1024 || h > 512) Fail("MDEC chunk: bad size " + std::to_string(w) + "x" + std::to_string(h));
    if (magic != kBsMagic) Fail("MDEC chunk: BS magic " + Hex(magic) + " != 0x3800");
    if (version != 2) Fail("MDEC chunk: BS version " + std::to_string(version) + " != 2");
    if (qscale < 1 || qscale > 63) Fail("MDEC chunk: qscale " + std::to_string(qscale) + " out of 1..63");

    const int mbw = (w + 15) / 16, mbh = (h + 15) / 16;
    const int expectBlocks = mbw * mbh * 6;
    width = w;
    height = h;
    rgb.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 3, 0);

    BitReader br(c.subspan(0x18));
    MdecFrameStats st;
    // The run-level halfwords the game's DctVlc (SLUS 0x80020400) hands the MDEC, one macroblock at a time, decoded
    // by the MDEC's own arithmetic (rr::mdec).
    std::vector<uint16_t> mbHalfwords;
    mbHalfwords.reserve(6 * 64);
    const rr::mdec::Model model = rr::mdec::ActiveModel();
    const rr::mdec::Tables& tables = book.MdecTables();
    uint32_t mbRgb[256];
    bool terminated = false;
    for (;;) {
        const uint32_t dc = br.Read(10);
        if (dc == kDcEnd) {
            terminated = true;
            break;
        }
        if (st.blocks >= expectBlocks) Fail("MDEC: no DC sentinel after " + std::to_string(st.blocks) + " blocks");
        const int bi = st.blocks % 6;
        const int mb = st.blocks / 6;
        ++st.blocks;
        ++st.emittedHalfwords;
        mbHalfwords.push_back(static_cast<uint16_t>((static_cast<uint32_t>(qscale) << 10) | (dc & 0x3FFu)));
        int k = 0;
        for (;;) {
            const uint8_t idx = book.lookup[br.Peek(kLookupBits)];
            if (idx == 0) Fail("MDEC: undefined codeword at bit " + std::to_string(br.Pos()));
            br.Skip(book.codeLength[idx - 1u]);
            uint16_t sym = book.symbols[idx - 1u];
            bool escaped = false;
            if (sym == kEscape) {
                sym = static_cast<uint16_t>(br.Read(16));
                escaped = true;
                ++st.escapes;
            }
            ++st.emittedHalfwords;
            mbHalfwords.push_back(sym);
            k += ((sym >> 10) & 0x3F) + 1;
            if (k > 63) {
                // The MDEC ends the block here; the game's decoder only ends it on a coded EOB.
                if (sym == kEob && !escaped) break;
                Fail("MDEC: run past coefficient 63 at bit " + std::to_string(br.Pos()));
            }
        }
        if (bi != 5) continue;
        rr::mdec::Input in{mbHalfwords.data(), mbHalfwords.size(), 0};
        if (!rr::mdec::DecodeColourMacroblock(in, tables, false, model, mbRgb))
            Fail("MDEC: macroblock " + std::to_string(mb) + " ran out of run-levels");
        mbHalfwords.clear();
        const int mx = mb / mbh, my = mb % mbh;  // column-major macroblock order
        for (int sy = 0; sy < 16; ++sy) {
            const int py = my * 16 + sy;
            if (py >= h) break;
            for (int sx = 0; sx < 16; ++sx) {
                const int px = mx * 16 + sx;
                if (px >= w) break;
                const uint32_t v = mbRgb[sy * 16 + sx];
                uint8_t* o = &rgb[(static_cast<size_t>(py) * static_cast<size_t>(w) + static_cast<size_t>(px)) * 3];
                o[0] = static_cast<uint8_t>(v);
                o[1] = static_cast<uint8_t>(v >> 8);
                o[2] = static_cast<uint8_t>(v >> 16);
            }
        }
    }
    if (!terminated || st.blocks != expectBlocks)
        Fail("MDEC: " + std::to_string(st.blocks) + " blocks decoded, header needs " + std::to_string(expectBlocks));
    // The game pads its halfword output with EOB to a 128-byte multiple and the header's nwords
    // counts that padded length in 32-bit words (video.md 2, step 3).
    const int expectNwords = (st.emittedHalfwords + 63) / 64 * 64 / 2;
    if (expectNwords != nwords)
        Fail("MDEC: " + std::to_string(expectNwords) + " MDEC words emitted, header says " + std::to_string(nwords));
    // Only padding may follow the halfword that holds the sentinel: on this disc 16..30 bytes of
    // 0xFF in every .STR/.TCM chunk (sizes are 16-byte multiples) and 0 or 2 bytes of 0x00 in every
    // .WVE chunk (4-byte multiples). Anything longer, mixed, or of another value is rejected.
    const size_t used = 0x18 + static_cast<size_t>((br.Pos() + 15) / 16) * 2;
    st.paddingBytes = static_cast<int>(c.size() - used);
    st.paddingValue = used < c.size() ? c[used] : -1;
    for (size_t i = used; i < c.size(); ++i)
        if (c[i] != c[used]) st.paddingValue = 0x100;
    if (st.paddingBytes >= 32 || (st.paddingValue != -1 && st.paddingValue != 0x00 && st.paddingValue != 0xFF))
        Fail("MDEC: " + std::to_string(st.paddingBytes) + " bytes after the bitstream are not uniform 0x00/0xFF padding");
    return st;
}

}  // namespace detail

Picture15 DecodeMdecChunk(std::span<const uint8_t> chunk, const MdecCodebook& book) {
    int w = 0, h = 0;
    std::vector<uint8_t> rgb;
    detail::DecodeMdecChunkRgb(chunk, book, w, h, rgb);
    return PackPicture(w, h, rgb);
}

std::vector<std::span<const uint8_t>> ListStrChunks(std::span<const uint8_t> file) {
    std::vector<std::span<const uint8_t>> out;
    size_t off = 0;
    bool first = true;
    while (off < file.size()) {
        if (file.size() - off < 8) Fail("chunk chain: trailing " + std::to_string(file.size() - off) + " bytes");
        const uint32_t size = U32be(file, off + 4);
        if (size < 8 || size > file.size() - off) Fail("chunk chain: bad chunk size at " + Hex(off));
        if (TagIs(file, off, "MDEC")) {
            out.push_back(file.subspan(off, size));
        } else if (TagIs(file, off, "VLC0")) {
            if (!first) Fail("chunk chain: VLC0 chunk is not the first one");
        } else if (!TagIs(file, off, "au00") && !TagIs(file, off, "au01")) {
            Fail("chunk chain: unknown tag at " + Hex(off));
        }
        first = false;
        off += size;
    }
    if (out.empty()) Fail("chunk chain: no MDEC chunk");
    return out;
}

MdecCodebook CodebookForStream(std::span<const uint8_t> file, const MdecCodebook& exeDefault) {
    if (!TagIs(file, 0, "VLC0")) return exeDefault;
    if (U32be(file, 4) != 8 + 2 * kMdecCodewords) Fail("VLC0 chunk: size " + std::to_string(U32be(file, 4)) + " != 456");
    std::array<uint16_t, kMdecCodewords> syms{};
    for (size_t i = 0; i < syms.size(); ++i) syms[i] = U16le(file, 8 + 2 * i);
    return exeDefault.WithSymbols(syms);
}

std::vector<Picture15> DecodeStrFrames(std::span<const uint8_t> file, const MdecCodebook& book) {
    const auto chunks = ListStrChunks(file);
    const MdecCodebook own = CodebookForStream(file, book);
    std::vector<Picture15> out;
    out.reserve(chunks.size());
    for (const auto& c : chunks) out.push_back(DecodeMdecChunk(c, own));
    return out;
}

std::vector<Picture15> DecodeTcmFrames(std::span<const uint8_t> file, const MdecCodebook& book) {
    if (!IsTcm(file)) Fail("TCM: not a TCM archive");
    const uint32_t count = U32le(file, 0);
    size_t expectOff = 4 + 8 * static_cast<size_t>(count);
    std::vector<Picture15> out;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t size = U32le(file, 4 + 8 * static_cast<size_t>(i));
        const uint32_t off = U32le(file, 8 + 8 * static_cast<size_t>(i));
        if (off != expectOff) Fail("TCM: entry " + std::to_string(i) + " is not contiguous");
        Need(file, off, size, "TCM entry");
        out.push_back(DecodeMdecChunk(file.subspan(off, size), book));
        expectOff = static_cast<size_t>(off) + size;
    }
    if (expectOff != file.size()) Fail("TCM: last entry does not end at the file size");
    return out;
}

// =============================================================================================
// SHPP shape bank
// =============================================================================================

const Shape* ShapeBank::Find(const char* fourcc) const {
    for (const auto& s : shapes)
        if (std::strncmp(s.name, fourcc, 4) == 0 && std::strlen(fourcc) == 4) return &s;
    return nullptr;
}

ShapeBank ParseShapeBank(std::span<const uint8_t> f) {
    if (!TagIs(f, 0, "SHPP")) Fail("SHPP: bad magic");
    if (U32le(f, 4) != f.size()) Fail("SHPP: size field " + std::to_string(U32le(f, 4)) + " != file size");
    const uint32_t count = U32le(f, 8);
    if (!TagIs(f, 0x0C, "GIMX")) Fail("SHPP: directory tag is not GIMX");
    if (count == 0 || 0x10 + 8ull * count > f.size()) Fail("SHPP: bad entry count");
    const size_t dirEnd = 0x10 + 8 * static_cast<size_t>(count);
    ShapeBank bank;
    for (uint32_t i = 0; i < count; ++i) {
        const size_t de = 0x10 + 8 * static_cast<size_t>(i);
        const uint32_t off = U32le(f, de + 4);
        const uint32_t next = i + 1 < count ? U32le(f, de + 12) : static_cast<uint32_t>(f.size());
        if (i == 0 && off != dirEnd) Fail("SHPP: first entry does not start where the directory ends");
        if (next <= off || next > f.size()) Fail("SHPP: entry " + std::to_string(i) + " offsets not ascending");
        Shape s;
        std::memcpy(s.name, f.data() + de, 4);
        const uint8_t code = f[off];
        if (code != 0x42) Fail("SHPP: entry code " + Hex(code) + " (only 0x42 = 16bpp is known)");
        s.width = U16le(f, off + 4);
        s.height = U16le(f, off + 6);
        const size_t px = static_cast<size_t>(s.width) * static_cast<size_t>(s.height);
        if (s.width == 0 || s.height == 0 || 16 + px * 2 != next - off)
            Fail(std::string("SHPP: entry ") + s.name + " size does not close on the next offset");
        s.px.resize(px);
        for (size_t p = 0; p < px; ++p) s.px[p] = U16le(f, off + 16 + 2 * p);
        bank.shapes.push_back(std::move(s));
    }
    return bank;
}

// =============================================================================================
// FNTP font
// =============================================================================================

const Glyph* Font::Find(uint32_t code) const {
    if (code < firstCode || code - firstCode >= glyphs.size()) return nullptr;
    return &glyphs[code - firstCode];
}

Font ParseFont(std::span<const uint8_t> f) {
    constexpr size_t kGlyphRecord = 11;
    if (!TagIs(f, 0, "FNTP")) Fail("FNTP: bad magic");
    if (U32le(f, 4) != f.size()) Fail("FNTP: size field != file size");
    const uint16_t count = U16le(f, 0x0A);
    Font font;
    font.lineHeight = f[0x13];
    font.firstCode = U32le(f, 0x14);
    const uint32_t bitmap = U32le(f, 0x1C);
    if (count == 0 || bitmap != 0x20 + kGlyphRecord * count) Fail("FNTP: glyph table does not end at the bitmap offset");
    for (uint16_t i = 0; i < count; ++i) {
        const size_t r = 0x20 + kGlyphRecord * i;
        Need(f, r, kGlyphRecord, "FNTP glyph");
        Glyph g{};
        g.code = U16le(f, r);
        g.w = f[r + 2];
        g.h = f[r + 3];
        g.x = U16le(f, r + 4);
        g.y = U16le(f, r + 6);
        g.advance = f[r + 8];
        g.xoff = static_cast<int8_t>(f[r + 9]);
        g.yoff = static_cast<int8_t>(f[r + 10]);
        if (g.code != font.firstCode + i) Fail("FNTP: glyph codes are not consecutive from the first code");
        font.glyphs.push_back(g);
    }
    font.sheetVramWords = U16le(f, bitmap);
    font.sheetWidth = U16le(f, bitmap + 4);
    font.sheetHeight = U16le(f, bitmap + 6);
    if (font.sheetWidth <= 0 || font.sheetHeight <= 0 || (font.sheetWidth & 1))
        Fail("FNTP: bad sheet size");
    if (font.sheetVramWords * 4 < font.sheetWidth) Fail("FNTP: sheet wider than its VRAM width");
    const size_t packed = static_cast<size_t>(font.sheetWidth) * static_cast<size_t>(font.sheetHeight) / 2;
    if (bitmap + 16 + packed != f.size()) Fail("FNTP: bitmap does not end at the file size");
    for (const Glyph& g : font.glyphs)
        if (g.x + g.w > font.sheetWidth || g.y + g.h > font.sheetHeight)
            Fail("FNTP: glyph " + std::to_string(g.code) + " box is outside the sheet");
    font.sheet.resize(packed * 2);
    for (size_t i = 0; i < packed; ++i) {
        const uint8_t b = f[bitmap + 16 + i];
        font.sheet[i * 2] = b & 0x0F;  // low nibble first (textures.md 0.2)
        font.sheet[i * 2 + 1] = static_cast<uint8_t>(b >> 4);
    }
    return font;
}

// =============================================================================================
// LOCH / LOCL string pool
// =============================================================================================

std::vector<std::string> ParseStringPool(std::span<const uint8_t> f) {
    if (!TagIs(f, 0, "LOCH")) Fail("LOC: bad LOCH magic");
    if (U32le(f, 4) != 0x14 || U32le(f, 8) != 0 || U32le(f, 0x0C) != 1 || U32le(f, 0x10) != 0x14)
        Fail("LOC: LOCH header fields are not {0x14, 0, 1, 0x14}");
    const std::span<const uint8_t> c = f.subspan(0x14);
    if (!TagIs(c, 0, "LOCL")) Fail("LOC: bad LOCL magic");
    if (U32le(c, 4) != c.size()) Fail("LOC: LOCL chunk size != file size - 0x14");
    if (U32le(c, 8) != 0) Fail("LOC: LOCL +8 is not 0");
    const uint32_t count = U32le(c, 0x0C);
    const size_t indexEnd = 0x10 + 4ull * count;
    if (count == 0 || indexEnd > c.size()) Fail("LOC: bad string count");
    if (U32le(c, 0x10) != indexEnd) Fail("LOC: the offset index does not end where the first string begins");
    std::vector<std::string> out;
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t off = U32le(c, 0x10 + 4 * static_cast<size_t>(i));
        if (off < indexEnd || off >= c.size()) Fail("LOC: string " + std::to_string(i) + " offset outside the chunk");
        const auto* begin = c.data() + off;
        const auto* end = static_cast<const uint8_t*>(std::memchr(begin, 0, c.size() - off));
        if (!end) Fail("LOC: string " + std::to_string(i) + " is not NUL-terminated inside the chunk");
        out.emplace_back(reinterpret_cast<const char*>(begin), static_cast<size_t>(end - begin));
    }
    return out;
}

// =============================================================================================
// Helpers
// =============================================================================================

uint32_t Bgr555ToRgba8(uint16_t px) {
    if (px == 0) return 0;
    const uint32_t r = px & 0x1F, g = (px >> 5) & 0x1F, b = (px >> 10) & 0x1F;
    return ((r << 3) | (r >> 2)) | (((g << 3) | (g >> 2)) << 8) | (((b << 3) | (b >> 2)) << 16) | (0xFFu << 24);
}

std::vector<uint8_t> ReadWholeFile(const std::string& path) {
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    if (!in) Fail("cannot open " + path);
    in.seekg(0, std::ios::end);
    const std::streamoff n = in.tellg();
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<size_t>(n));
    if (n > 0 && !in.read(reinterpret_cast<char*>(data.data()), n)) Fail("cannot read " + path);
    return data;
}

// =============================================================================================
// Verification bench
// =============================================================================================

namespace {

constexpr int kVramW = 1024, kVramH = 512;

struct Vram {
    std::vector<uint16_t> hw;  // 1024 x 512 halfwords
    uint16_t At(int x, int y) const { return hw[static_cast<size_t>(y) * kVramW + static_cast<size_t>(x & (kVramW - 1))]; }
};

std::string Fmt(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return buf;
}

int ChanDiff(uint16_t a, uint16_t b) {
    const int dr = std::abs(static_cast<int>(a & 31) - static_cast<int>(b & 31));
    const int dg = std::abs(static_cast<int>((a >> 5) & 31) - static_cast<int>((b >> 5) & 31));
    const int db = std::abs(static_cast<int>((a >> 10) & 31) - static_cast<int>((b >> 10) & 31));
    const int stp = ((a ^ b) & 0x8000) ? 99 : 0;
    return std::max({dr, dg, db, stp});
}

// Exact search for a halfword image (w x h). Uses the row with the most distinct values as the key.
struct ExactHit {
    int x = -1, y = -1;
    size_t equal = 0;
};
ExactHit FindExact(const Vram& v, const std::vector<uint16_t>& img, int w, int h) {
    int keyRow = 0;
    size_t best = 0;
    for (int r = 0; r < h; ++r) {
        std::vector<uint16_t> row(img.begin() + static_cast<ptrdiff_t>(r) * w, img.begin() + static_cast<ptrdiff_t>(r + 1) * w);
        std::sort(row.begin(), row.end());
        const size_t distinct = static_cast<size_t>(std::unique(row.begin(), row.end()) - row.begin());
        if (distinct > best) best = distinct, keyRow = r;
    }
    ExactHit hit;
    for (int y = 0; y + h <= kVramH; ++y)
        for (int x = 0; x < kVramW; ++x) {
            bool ok = true;
            for (int i = 0; i < w && ok; ++i) ok = v.At(x + i, y + keyRow) == img[static_cast<size_t>(keyRow) * w + i];
            if (!ok) continue;
            size_t eq = 0;
            for (int r = 0; r < h; ++r)
                for (int i = 0; i < w; ++i) eq += v.At(x + i, y + r) == img[static_cast<size_t>(r) * w + i];
            if (eq > hit.equal) hit = {x, y, eq};
        }
    return hit;
}

struct Approx {
    int x = 0, y = 0, w = 0, h = 0;
    int frame = 0;
    double exact = 0, within1 = 0, within2 = 0;
    int maxDiff = 0;
};

// Approximate search for an MDEC picture: 128 sampled pixels, channel tolerance 2 (5-bit), up to
// 8 misses; survivors get a full comparison and must be >= 90% within tolerance.
std::vector<Approx> FindApprox(const Vram& v, const Picture15& p, int frame) {
    std::vector<std::pair<int, int>> samples;
    for (int j = 0; j < 8; ++j)
        for (int i = 0; i < 16; ++i) samples.push_back({i * (p.width - 1) / 15, j * (p.height - 1) / 7});
    std::vector<Approx> hits;
    for (int y = 0; y + p.height <= kVramH; ++y)
        for (int x = 0; x < kVramW; ++x) {
            int miss = 0;
            for (const auto& [sx, sy] : samples) {
                if (ChanDiff(v.At(x + sx, y + sy), p.px[static_cast<size_t>(sy) * p.width + sx]) > 2 && ++miss > 8) break;
            }
            if (miss > 8) continue;
            size_t e0 = 0, e1 = 0, e2 = 0;
            int md = 0;
            for (int r = 0; r < p.height; ++r)
                for (int i = 0; i < p.width; ++i) {
                    const int d = ChanDiff(v.At(x + i, y + r), p.px[static_cast<size_t>(r) * p.width + i]);
                    e0 += d == 0, e1 += d <= 1, e2 += d <= 2;
                    md = std::max(md, d);
                }
            const double n = static_cast<double>(p.px.size());
            if (e2 < 0.9 * n) continue;
            hits.push_back({x, y, p.width, p.height, frame, 100.0 * static_cast<double>(e0) / n, 100.0 * static_cast<double>(e1) / n,
                            100.0 * static_cast<double>(e2) / n, md});
        }
    // keep local bests
    std::sort(hits.begin(), hits.end(), [](const Approx& a, const Approx& b) { return a.exact != b.exact ? a.exact > b.exact : a.within1 > b.within1; });
    std::vector<Approx> kept;
    for (const auto& h : hits) {
        bool dup = false;
        for (const auto& k : kept)
            if (std::abs(k.x - h.x) < p.width / 2 && std::abs(k.y - h.y) < p.height / 2) dup = true;
        if (!dup) kept.push_back(h);
    }
    return kept;
}

struct Expect {
    const char* name;
    int frames;
    int w, h;  // 0 = not stated by the doc
};

// video.md section 6 (counts and sizes only).
constexpr Expect kExpected[] = {
    {"BGRND3.STR", 1, 304, 144},   {"TTL_CON2.STR", 1, 224, 32},  {"BGRND2.STR", 4, 256, 128},
    {"LEGAL.STR", 4, 256, 128},    {"LOSE.STR", 5, 256, 112},     {"WIN.STR", 5, 0, 0},
    {"WRECK.STR", 5, 0, 0},        {"ESCAPE.STR", 6, 0, 0},       {"FIVEO.STR", 6, 0, 0},
    {"ALIAS.STR", 8, 0, 0},        {"JAILED.STR", 8, 0, 0},       {"DES_LOSE.STR", 10, 0, 0},
    {"DES_WIN.STR", 10, 0, 0},     {"KAF_LOSE.STR", 10, 0, 0},    {"KAF_WIN.STR", 10, 0, 0},
    {"BUST.STR", 11, 0, 0},        {"CNTRLLR.STR", 12, 256, 128}, {"MISCRES.STR", 12, 0, 0},
    {"MODES.STR", 13, 0, 0},       {"BIKES.STR", 21, 0, 0},       {"TITLES.STR", 23, 336, 32},
    {"MODEH2H.STR", 30, 0, 0},     {"MODEJB.STR", 30, 0, 0},      {"MODETT.STR", 30, 0, 0},
    {"MODESCAR.STR", 31, 0, 0},    {"TROPHY.STR", 31, 0, 0},      {"OPTIONS.STR", 34, 0, 0},
    {"SSCOURSE.STR", 36, 0, 0},    {"MODEFIVO.STR", 44, 0, 0},    {"RR_LOGO.STR", 45, 0, 0},
    {"FSCOURSE.STR", 57, 0, 0},    {"MODECNR.STR", 60, 0, 0},     {"MODEH2HS.STR", 120, 0, 0},
    {"COP1.STR", 91, 256, 112},    {"COP2.STR", 91, 256, 112},    {"COP3.STR", 91, 256, 112},
    {"CRUISEA1.STR", 91, 256, 112}, {"CRUISEA2.STR", 91, 256, 112}, {"CRUISEA3.STR", 91, 256, 112},
    {"CRUISEB1.STR", 91, 256, 112}, {"CRUISEB2.STR", 91, 256, 112}, {"CRUISEB3.STR", 91, 256, 112},
    {"CRUISES1.STR", 91, 256, 112}, {"CRUISES2.STR", 91, 256, 112}, {"CRUISES3.STR", 91, 256, 112},
    {"SPORTA1.STR", 91, 256, 112}, {"SPORTA2.STR", 91, 256, 112}, {"SPORTA3.STR", 91, 256, 112},
    {"SPORTB1.STR", 91, 256, 112}, {"SPORTB2.STR", 91, 256, 112}, {"SPORTB3.STR", 91, 256, 112},
    {"SPORTS1.STR", 91, 256, 112}, {"SPORTS2.STR", 91, 256, 112}, {"SPORTS3.STR", 91, 256, 112},
    {"EA_LOGO.WVE", 75, 320, 224}, {"CREDITS.WVE", 266, 320, 192}, {"GAUNTLET.WVE", 356, 320, 192},
    {"MOVINGUP.WVE", 443, 320, 192}, {"BUSTED.WVE", 491, 320, 192}, {"SPAZPUNT.WVE", 522, 320, 192},
    {"THESETUP.WVE", 531, 320, 192}, {"GANGS.WVE", 536, 320, 192}, {"INTRO.WVE", 632, 320, 224},
    {"GOT_OINK.WVE", 728, 320, 192}, {"JAILBRAK.WVE", 767, 320, 192}, {"FSLOAD.TCM", 64, 384, 240},
    {"SSLOAD.TCM", 36, 384, 240},
};

std::string Upper(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}

struct FileResult {
    std::string name;
    std::string line;
    std::vector<Approx> hits;
    int frames = 0;
    bool ok = false;
    std::string sizes;
};

}  // namespace

std::string CheckShellAssets(const std::string& discExtractDir, const std::string& stateDir) {
    namespace fs = std::filesystem;
    std::ostringstream rep;
    const fs::path root(discExtractDir);
    const fs::path fe = root / "DATA" / "FE";

    // ---- VRAM capture
    Vram vram;
    {
        const auto raw = ReadWholeFile((fs::path(stateDir) / "vram.bin").string());
        if (raw.size() != kVramW * kVramH * 2) Fail("vram.bin is not 1 MiB");
        // The retro-shell vram.bin is NOT big-endian: it is the little-endian VRAM image extracted one
        // byte late, so halfword k is raw[2k-1] | raw[2k] << 8 (the low byte of halfword 0 is lost and
        // the last file byte is foreign). Read as big-endian it only looks right because each
        // halfword's high byte (blue + top green bits) lands in the correct place. Proven by all 26
        // FEMISC shapes occurring as raw little-endian byte runs at odd file offsets.
        vram.hw.resize(kVramW * kVramH);
        for (size_t i = 0; i < vram.hw.size(); ++i)
            vram.hw[i] = static_cast<uint16_t>((i ? raw[i * 2 - 1] : 0) | (raw[i * 2] << 8));
    }

    // ---- (a) string pools
    rep << "== String pools ==\n";
    for (const auto& [rel, want] : {std::pair<fs::path, size_t>{fs::path("DATA") / "FE" / "FESTRING.LOC", 1945},
                                    std::pair<fs::path, size_t>{fs::path("DATA") / "GAMESTRG.LOC", 181}}) {
        try {
            const auto d = ReadWholeFile((root / rel).string());
            const auto pool = ParseStringPool(d);
            size_t empty = 0, longest = 0;
            for (const auto& s : pool) empty += s.empty(), longest = std::max(longest, s.size());
            rep << Fmt("  %-22s %6zu bytes  %4zu strings (doc %zu) %s  empty=%zu longest=%zu\n", rel.string().c_str(),
                       d.size(), pool.size(), want, pool.size() == want ? "OK" : "MISMATCH", empty, longest);
        } catch (const std::exception& e) {
            rep << "  " << rel.string() << " FAILED: " << e.what() << "\n";
        }
    }

    // ---- (a) FEMISC.PSH, compared with VRAM
    rep << "\n== FEMISC.PSH shapes vs capture VRAM (exact search) ==\n";
    try {
        const auto d = ReadWholeFile((fe / "FEMISC.PSH").string());
        const ShapeBank bank = ParseShapeBank(d);
        size_t totalPx = 0, totalEq = 0;
        int exactShapes = 0;
        for (const auto& s : bank.shapes) {
            const ExactHit h = FindExact(vram, s.px, s.width, s.height);
            const size_t n = s.px.size();
            totalPx += n;
            totalEq += h.equal;
            exactShapes += h.equal == n;
            if (h.x < 0)
                rep << Fmt("  %-4s %3dx%-3d  not found\n", s.name, s.width, s.height);
            else
                rep << Fmt("  %-4s %3dx%-3d  VRAM (%4d,%3d)  %6.2f%% exact\n", s.name, s.width, s.height, h.x, h.y,
                           100.0 * static_cast<double>(h.equal) / static_cast<double>(n));
        }
        rep << Fmt("  %zu shapes parsed, %d byte-exact in VRAM, %.3f%% of all shape pixels equal\n", bank.shapes.size(),
                   exactShapes, 100.0 * static_cast<double>(totalEq) / static_cast<double>(totalPx));
    } catch (const std::exception& e) {
        rep << "  FEMISC.PSH FAILED: " << e.what() << "\n";
    }

    // ---- (a) fonts, sheets located in VRAM as packed 4bpp
    rep << "\n== Fonts (4bpp sheets vs capture VRAM) ==\n";
    const fs::path fonts[] = {root / "DATA" / "GAMEFONT.PFN", fe / "MINIFONT.PFN", fe / "BTN_FONT.PFN", fe / "HDR_FONT.PFN"};
    struct SheetAt {
        int x, y, w, h;
    };
    std::vector<SheetAt> sheets;
    for (const auto& path : fonts) {
        try {
            const auto d = ReadWholeFile(path.string());
            const Font f = ParseFont(d);
            const int words = f.sheetWidth / 4;
            std::vector<uint16_t> packed(static_cast<size_t>(words) * static_cast<size_t>(f.sheetHeight));
            for (size_t i = 0; i < packed.size(); ++i) {
                uint16_t v = 0;
                for (int n = 0; n < 4; ++n) v = static_cast<uint16_t>(v | (f.sheet[i * 4 + static_cast<size_t>(n)] << (4 * n)));
                packed[i] = v;
            }
            const ExactHit h = FindExact(vram, packed, words, f.sheetHeight);
            rep << Fmt("  %-12s glyphs=%zu codes %u..%u line=%d sheet %dx%d (%d VRAM words)", path.filename().string().c_str(),
                       f.glyphs.size(), f.firstCode, f.firstCode + static_cast<uint32_t>(f.glyphs.size()) - 1, f.lineHeight,
                       f.sheetWidth, f.sheetHeight, f.sheetVramWords);
            if (h.x < 0) {
                rep << "  not in VRAM\n";
            } else {
                rep << Fmt("  VRAM (%d,%d) %.2f%% exact\n", h.x, h.y,
                           100.0 * static_cast<double>(h.equal) / static_cast<double>(packed.size()));
                sheets.push_back({h.x, h.y, words, f.sheetHeight});
            }
        } catch (const std::exception& e) {
            rep << "  " << path.filename().string() << " FAILED: " << e.what() << "\n";
        }
    }
    // The font files carry no CLUT. Candidates: every 16-halfword run in VRAM with entry 0 == 0
    // (transparent), entries 1..15 non-zero and distinct, and each of R, G, B non-decreasing with
    // the index (a ramp). Reported when within 64 pixels of a located sheet.
    for (int y = 0; y < kVramH; ++y)
        for (int x = 0; x + 16 <= kVramW; ++x) {
            uint16_t e[16];
            for (int i = 0; i < 16; ++i) e[i] = vram.At(x + i, y);
            if (e[0] != 0) continue;
            bool ok = true;
            for (int i = 1; i < 16 && ok; ++i) {
                if (e[i] == 0) ok = false;
                for (int j = 1; j < i && ok; ++j) ok = e[i] != e[j];
                for (int s = 0; s <= 10 && ok && i > 1; s += 5) ok = ((e[i] >> s) & 31) >= ((e[i - 1] >> s) & 31);
            }
            if (!ok) continue;
            bool near = false;
            for (const auto& sh : sheets)
                near |= x >= sh.x - 64 && x < sh.x + sh.w + 64 && y >= sh.y - 64 && y < sh.y + sh.h + 64;
            if (!near) continue;
            int stp = 0;
            for (int i = 0; i < 16; ++i) stp += (e[i] >> 15) & 1;
            std::string ents;
            for (int i = 0; i < 16; ++i) ents += Fmt(" %04X", e[i]);
            rep << Fmt("  CLUT candidate (ramp) at VRAM (%d,%d)%s  x%%16=%d, STP set on %d entries:%s\n", x, y,
                       x % 16 ? " [NOT 16-aligned: not addressable as a GPU CLUT id]" : "", x % 16, stp, ents.c_str());
        }

    // ---- (b)+(c) MDEC
    rep << "\n== MDEC containers in DATA\\FE ==\n";
    std::vector<uint8_t> slus;
    MdecCodebook book;
    try {
        slus = ReadWholeFile((root / "SLUS_010.53").string());
        book = LoadDefaultCodebook(slus);
    } catch (const std::exception& e) {
        rep << "  code book FAILED: " << e.what() << "\n";
        return rep.str();
    }
    std::vector<fs::path> files;
    for (const auto& de : fs::directory_iterator(fe)) {
        const std::string ext = Upper(de.path().extension().string());
        if (ext == ".STR" || ext == ".WVE" || ext == ".TCM") files.push_back(de.path());
    }
    std::sort(files.begin(), files.end());
    std::vector<FileResult> results(files.size());
    std::atomic<size_t> next{0};
    auto worker = [&]() {
        for (size_t i = next++; i < files.size(); i = next++) {
            FileResult& r = results[i];
            r.name = Upper(files[i].filename().string());
            const std::string ext = Upper(files[i].extension().string());
            try {
                const auto d = ReadWholeFile(files[i].string());
                std::map<std::pair<int, int>, int> sizes;
                auto account = [&](const Picture15& p) { ++sizes[{p.width, p.height}]; };
                if (ext == ".TCM") {
                    const auto pics = DecodeTcmFrames(d, book);
                    for (const auto& p : pics) account(p);
                    r.frames = static_cast<int>(pics.size());
                } else {
                    const auto chunks = ListStrChunks(d);
                    const MdecCodebook own = CodebookForStream(d, book);
                    for (size_t k = 0; k < chunks.size(); ++k) {
                        const Picture15 p = DecodeMdecChunk(chunks[k], own);
                        account(p);
                        if (ext == ".STR") {
                            auto hits = FindApprox(vram, p, static_cast<int>(k));
                            r.hits.insert(r.hits.end(), hits.begin(), hits.end());
                        }
                    }
                    r.frames = static_cast<int>(chunks.size());
                }
                for (const auto& [wh, n] : sizes) r.sizes += Fmt("%s%dx%d", r.sizes.empty() ? "" : ",", wh.first, wh.second);
                const Expect* ex = nullptr;
                for (const auto& e : kExpected)
                    if (r.name == e.name) ex = &e;
                bool ok = ex && ex->frames == r.frames;
                if (ex && ex->w && (sizes.size() != 1 || sizes.begin()->first != std::make_pair(ex->w, ex->h))) ok = false;
                r.ok = ok;
                r.line = Fmt("  %-13s %9zu bytes %4d frames %-9s  doc: %s", r.name.c_str(), d.size(), r.frames, r.sizes.c_str(),
                             ex ? (ok ? "OK" : "MISMATCH") : "not listed");
            } catch (const std::exception& e) {
                r.line = "  " + r.name + " FAILED: " + e.what();
            }
        }
    };
    {
        std::vector<std::thread> pool;
        const unsigned n = std::max(1u, std::thread::hardware_concurrency());
        for (unsigned t = 0; t < n; ++t) pool.emplace_back(worker);
        for (auto& t : pool) t.join();
    }
    int okFiles = 0, totalFrames = 0;
    for (const auto& r : results) {
        rep << r.line << "\n";
        okFiles += r.ok;
        totalFrames += r.frames;
    }
    rep << Fmt("  %d/%zu files match video.md section 6, %d frames decoded strictly\n", okFiles, results.size(), totalFrames);

    rep << "\n== MDEC pictures found in capture VRAM (tolerance 2/31 per channel) ==\n";
    std::vector<std::pair<std::string, Approx>> all;
    for (auto& r : results) {
        // one line per VRAM location: the best frame there
        std::sort(r.hits.begin(), r.hits.end(), [](const Approx& a, const Approx& b) { return a.exact != b.exact ? a.exact > b.exact : a.within1 > b.within1; });
        std::vector<Approx> kept;
        for (const auto& h : r.hits) {
            bool dup = false;
            for (const auto& k : kept)
                if (std::abs(k.x - h.x) < 32 && std::abs(k.y - h.y) < 16) dup = true;
            if (!dup) kept.push_back(h);
        }
        for (const auto& h : kept) all.push_back({r.name, h});
    }
    size_t px = 0, e0 = 0, e1 = 0;
    for (const auto& [name, h] : all) {
        std::string over;
        for (const auto& [n2, o] : all) {
            if (&o == &h) continue;
            // horizontal overlap on the 1024-wide wrapping x axis
            const int dx = ((o.x - h.x) % kVramW + kVramW) % kVramW;
            const bool xo = dx < h.w || kVramW - dx < o.w;
            if (xo && o.y < h.y + h.h && h.y < o.y + o.h) over += Fmt(" %s#%d@(%d,%d)", n2.c_str(), o.frame, o.x, o.y);
        }
        rep << Fmt("  %-13s frame %3d %3dx%-3d at VRAM (%4d,%3d): exact %6.2f%%  |d|<=1 %6.2f%%  |d|<=2 %6.2f%%  max|d| %2d%s%s\n",
                   name.c_str(), h.frame, h.w, h.h, h.x, h.y, h.exact, h.within1, h.within2, h.maxDiff,
                   over.empty() ? "" : "  overlapped by", over.c_str());
        if (over.empty()) {
            const size_t n = static_cast<size_t>(h.w) * static_cast<size_t>(h.h);
            px += n;
            e0 += static_cast<size_t>(h.exact / 100.0 * static_cast<double>(n) + 0.5);
            e1 += static_cast<size_t>(h.within1 / 100.0 * static_cast<double>(n) + 0.5);
        }
    }
    if (px)
        rep << Fmt("  pictures not overlapped by another hit: %zu pixels, %.2f%% exact, %.2f%% within 1/31 per channel\n", px,
                   100.0 * static_cast<double>(e0) / static_cast<double>(px), 100.0 * static_cast<double>(e1) / static_cast<double>(px));
    return rep.str();
}

}  // namespace rr::shell
