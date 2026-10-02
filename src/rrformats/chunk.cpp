#include "rrformats/chunk.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>

namespace rr {
namespace {

constexpr size_t kWindowListEnd = 0x1C;   // where the 0xFFFE/0xFFFF terminator sits
constexpr size_t kSubBlockChain = 0x8C;   // fixed for all 226 type-3 objects of both sets
constexpr size_t kSliceRecord = 52;
constexpr size_t kXsaiRecord = 20;

uint16_t ReadU16(std::span<const uint8_t> d, size_t off) {
    if (off + 2 > d.size()) throw std::runtime_error("chunk: read past end of chunk");
    return static_cast<uint16_t>(d[off] | (d[off + 1] << 8));
}

int16_t ReadS16(std::span<const uint8_t> d, size_t off) { return static_cast<int16_t>(ReadU16(d, off)); }

uint32_t ReadU32(std::span<const uint8_t> d, size_t off) {
    if (off + 4 > d.size()) throw std::runtime_error("chunk: read past end of chunk");
    return static_cast<uint32_t>(d[off]) | (static_cast<uint32_t>(d[off + 1]) << 8) |
           (static_cast<uint32_t>(d[off + 2]) << 16) | (static_cast<uint32_t>(d[off + 3]) << 24);
}

int32_t ReadS32(std::span<const uint8_t> d, size_t off) { return static_cast<int32_t>(ReadU32(d, off)); }

bool TagIs(std::span<const uint8_t> d, size_t off, const char* tag) {
    return off + 4 <= d.size() && std::memcmp(d.data() + off, tag, 4) == 0;
}

} // namespace

ChunkHeader ParseChunkHeader(std::span<const uint8_t> chunk) {
    if (chunk.size() < kChunkSize) throw std::runtime_error("chunk: short chunk");
    ChunkHeader header;
    header.key = ReadU32(chunk, 0);
    header.type = static_cast<uint8_t>(header.key >> 28);
    header.id = header.key & 0x0FFFFFFFu;
    header.group = static_cast<uint16_t>(header.id >> 16);
    header.index = static_cast<uint16_t>(header.id & 0xFFFF);
    for (size_t off = 0x04; off + 6 <= kWindowListEnd; off += 6) {
        const uint16_t road = ReadU16(chunk, off);
        if (road == 0xFFFF || road == 0xFFFE) continue; // empty slot / terminator
        ResidencyWindow w;
        w.road = road;
        w.from = ReadU16(chunk, off + 2);
        w.to = ReadU16(chunk, off + 4);
        header.windows.push_back(w);
    }
    return header;
}

RoadObject ParseRoadChunk(std::span<const uint8_t> chunk) {
    RoadObject object;
    object.header = ParseChunkHeader(chunk);
    if (object.header.type != static_cast<uint8_t>(ChunkType::Road))
        throw std::runtime_error("chunk: not a road chunk (type " + std::to_string(object.header.type) + ")");

    object.pieceKey = ReadU32(chunk, 0x24);
    object.headerOwner = ReadU32(chunk, 0x28);
    object.half = ReadU32(chunk, 0x2C);

    // Sub-block chain: char tag[4]; u32 size; walked by offset += size.
    size_t at = kSubBlockChain;
    while (at + 8 <= chunk.size()) {
        const uint32_t size = ReadU32(chunk, at + 4);
        if (size < 8 || at + size > chunk.size()) break; // end of the chain, or a continuation chunk
        const size_t body = at + 8;
        const size_t bodySize = size - 8;

        if (TagIs(chunk, at, "GRPT")) {
            object.grptFlags = ReadU32(chunk, body + 0x00);
            object.ownerRoad = ReadU32(chunk, body + 0x0C);
            object.startAlongRoad = ReadU32(chunk, body + 0x18);
            object.endAlongRoad = ReadU32(chunk, body + 0x1C);
        } else if (TagIs(chunk, at, "SLCT")) {
            if (bodySize % kSliceRecord != 0)
                throw std::runtime_error("chunk: SLCT size is not a multiple of the slice record");
            const size_t count = bodySize / kSliceRecord;
            object.slices.reserve(count);
            for (size_t i = 0; i < count; ++i) {
                const size_t o = body + i * kSliceRecord;
                RoadSlice slice;
                slice.index = ReadU16(chunk, o + 0x00);
                for (size_t k = 0; k < 9; ++k) slice.m[k] = ReadS16(chunk, o + 0x02 + k * 2);
                for (size_t k = 0; k < 3; ++k) slice.pos[k] = ReadS32(chunk, o + 0x14 + k * 4);
                slice.chord = ReadU32(chunk, o + 0x20);
                slice.distance = ReadU32(chunk, o + 0x28);
                object.slices.push_back(slice);
            }
        } else if (TagIs(chunk, at, "XSAI")) {
            if (bodySize >= kXsaiRecord) {
                object.laneCount = ReadU16(chunk, body + 0x02);
                object.laneWidth = static_cast<float>(ReadU32(chunk, body + 0x10)) / 65536.0f;
            }
        }
        at += size;
    }

    // Split the slices into runs. `distance + chord == next.distance` holds inside a run only, so a
    // junction object's disjoint arms separate themselves without a heuristic.
    // The sum is not exact - the authored values carry rounding - so the break is detected with a
    // relative tolerance. A real break is a jump of a whole arm's length, orders of magnitude larger
    // than the rounding, so the threshold is not delicate.
    size_t runStart = 0;
    for (size_t i = 0; i + 1 < object.slices.size(); ++i) {
        const double expected =
            static_cast<double>(object.slices[i].distance) + static_cast<double>(object.slices[i].chord);
        const double actual = static_cast<double>(object.slices[i + 1].distance);
        const double scale = static_cast<double>(object.slices[i].chord);
        const bool continues = scale > 0.0 && std::abs(expected - actual) <= scale * 0.01;
        if (!continues) {
            object.runs.push_back({runStart, i + 1 - runStart});
            runStart = i + 1;
        }
    }
    if (!object.slices.empty()) object.runs.push_back({runStart, object.slices.size() - runStart});
    return object;
}

// ------------------------------------------------------------------ type 4: the panorama
namespace {

// The EXE's load address and header size, so a guest address can be turned into a file offset.
constexpr uint32_t kExeLoad = 0x80010000;
constexpr size_t kExeHeader = 0x800;
constexpr uint32_t kVaShortCodes = 0x800528A0;     // 96 x {u8 len; u8 pad; u16 code << (16 - len)}
constexpr uint32_t kVaLongCodes = 0x80052A20;      // 128 x the same, after eight leading zero bits
constexpr uint32_t kVaDefaultSymbols = 0x80052C20; // 224 x u16, already in MDEC halfword form
constexpr int kShortCount = 96;
constexpr int kLongCount = 128;
constexpr uint16_t kMdecEob = 0xFE00;
constexpr uint16_t kMdecEscape = 0x7C1F;
constexpr int kMdecDcEnd = 0x1FF;

size_t ExeOffset(uint32_t va) { return static_cast<size_t>(va - kExeLoad) + kExeHeader; }

const int kZigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                         12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                         35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                         58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

// The MPEG-1 default intra matrix with entry 0 replaced by 2: the MDEC does not scale the DC term
// by qscale. docs\formats\video.md section 4.
const int kQuant[64] = {2,  16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37,
                        19, 22, 26, 27, 29, 34, 34, 38, 22, 22, 26, 27, 29, 34, 37, 40,
                        22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32, 35, 40, 48, 58,
                        26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83};

const double* CosTable() {
    static double table[64];
    static const bool built = [] {
        for (int x = 0; x < 8; ++x)
            for (int u = 0; u < 8; ++u)
                table[x * 8 + u] = std::cos((2.0 * x + 1.0) * u * 3.14159265358979323846 / 16.0) *
                                   (u == 0 ? std::sqrt(0.5) : 1.0);
        return true;
    }();
    (void)built;
    return table;
}

void Idct8x8(const int* in, double* out) {
    const double* c = CosTable();
    double tmp[64];
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            double sum = 0.0;
            for (int u = 0; u < 8; ++u)
                if (in[y * 8 + u] != 0) sum += in[y * 8 + u] * c[x * 8 + u];
            tmp[y * 8 + x] = sum;
        }
    for (int x = 0; x < 8; ++x)
        for (int y = 0; y < 8; ++y) {
            double sum = 0.0;
            for (int u = 0; u < 8; ++u) sum += tmp[u * 8 + x] * c[y * 8 + u];
            out[y * 8 + x] = sum * 0.25;
        }
}

uint8_t ClampByte(double v) {
    if (v <= 0.0) return 0;
    if (v >= 255.0) return 255;
    return static_cast<uint8_t>(v);
}

// The bitstream is a run of 16-bit little-endian words whose bits are consumed MSB first, so the
// two bytes of every word are swapped once and then it is a plain MSB-first bit stream.
class BitReader {
public:
    BitReader(const uint8_t* data, size_t size) : bytes_(size + 8, 0), bits_(size * 8) {
        for (size_t i = 0; i + 1 < size; i += 2) {
            bytes_[i] = data[i + 1];
            bytes_[i + 1] = data[i];
        }
    }
    uint32_t Peek(int count) const {
        const size_t byte = pos_ >> 3;
        uint64_t window = 0;
        for (int i = 0; i < 5; ++i) window = (window << 8) | bytes_[byte + static_cast<size_t>(i)];
        return static_cast<uint32_t>((window >> (40 - (pos_ & 7) - static_cast<size_t>(count))) &
                                     ((1u << count) - 1u));
    }
    uint32_t Read(int count) {
        const uint32_t value = Peek(count);
        pos_ += static_cast<size_t>(count);
        return value;
    }
    void Skip(int count) { pos_ += static_cast<size_t>(count); }
    bool Exhausted() const { return pos_ >= bits_; }

private:
    std::vector<uint8_t> bytes_;
    size_t bits_ = 0;
    size_t pos_ = 0;
};

int Signed10(uint32_t v) { return (v & 0x200u) ? static_cast<int>(v) - 1024 : static_cast<int>(v); }

// One PS1 BS frame of `width` x `height` pixels, decoded to RGB. Macroblock order is column-major
// and the six blocks of a macroblock are Cr, Cb, Y1..Y4 (docs\formats\video.md section 2).
void DecodeBsFrame(const MdecCodebook& book, const uint8_t* bs, size_t bsSize, int width, int height,
                   int qscale, std::vector<uint8_t>& rgb) {
    rgb.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 3, 0);
    const int mbw = (width + 15) / 16, mbh = (height + 15) / 16;
    BitReader reader(bs, bsSize);
    int coeff[64];
    double plane[6][64];
    int blocks = 0;
    while (true) {
        const uint32_t dc = reader.Read(10);
        if (static_cast<int>(dc) == kMdecDcEnd) break;
        if (blocks >= mbw * mbh * 6 || reader.Exhausted())
            throw std::runtime_error("chunk: MDEC bitstream ran past its block count");
        const int bi = blocks % 6;
        const int mb = blocks / 6;
        const int mx = mb / mbh, my = mb % mbh;
        ++blocks;
        std::memset(coeff, 0, sizeof(coeff));
        coeff[0] = Signed10(dc) * kQuant[0];
        int k = 0;
        while (true) {
            const uint32_t entry = book.lut[reader.Peek(book.maxLength)];
            if (entry == 0) throw std::runtime_error("chunk: undefined MDEC codeword");
            reader.Skip(static_cast<int>(entry >> 16));
            uint16_t symbol = static_cast<uint16_t>(entry & 0xFFFFu);
            if (symbol == kMdecEscape) symbol = static_cast<uint16_t>(reader.Read(16));
            if (symbol == kMdecEob) break;
            const int run = (symbol >> 10) & 0x3F;
            const int level = Signed10(symbol & 0x3FFu);
            k += run + 1;
            if (k > 63) throw std::runtime_error("chunk: MDEC run past coefficient 63");
            const int z = kZigzag[k];
            coeff[z] = (level * kQuant[z] * qscale) >> 3;
            if (k == 63) break;
        }
        Idct8x8(coeff, plane[bi]);
        if (bi != 5) continue;
        for (int sy = 0; sy < 16; ++sy) {
            const int py = my * 16 + sy;
            if (py >= height) continue;
            for (int sx = 0; sx < 16; ++sx) {
                const int px = mx * 16 + sx;
                if (px >= width) continue;
                const int ci = (sy >> 1) * 8 + (sx >> 1);
                const double cb = plane[1][ci], cr = plane[0][ci];
                const double y = plane[2 + (sy >> 3) * 2 + (sx >> 3)][(sy & 7) * 8 + (sx & 7)] + 128.0;
                const size_t o =
                    (static_cast<size_t>(py) * static_cast<size_t>(width) + static_cast<size_t>(px)) * 3;
                rgb[o + 0] = ClampByte(y + 1.402 * cr);
                rgb[o + 1] = ClampByte(y - 0.3437 * cb - 0.7143 * cr);
                rgb[o + 2] = ClampByte(y + 1.772 * cb);
            }
        }
    }
    if (blocks != mbw * mbh * 6)
        throw std::runtime_error("chunk: MDEC frame ended after " + std::to_string(blocks) +
                                 " blocks, expected " + std::to_string(mbw * mbh * 6));
}

} // namespace

MdecCodebook MdecCodebook::FromExe(std::span<const uint8_t> exe) {
    struct Word {
        int length;
        uint32_t bits;
    };
    std::vector<Word> words;
    words.reserve(static_cast<size_t>(kShortCount + kLongCount));
    const auto descriptor = [&](uint32_t va, int count, int leadingZeros) {
        const size_t at = ExeOffset(va);
        for (int i = 0; i < count; ++i) {
            const size_t record = at + static_cast<size_t>(i) * 4;
            if (record + 4 > exe.size()) throw std::runtime_error("chunk: EXE too short for the MDEC tables");
            const int length = exe[record];
            const uint32_t code = static_cast<uint32_t>(exe[record + 2] | (exe[record + 3] << 8));
            if (length < 1 || length > 16) throw std::runtime_error("chunk: bad MDEC codeword length");
            words.push_back({length + leadingZeros, code >> (16 - length)});
        }
    };
    descriptor(kVaShortCodes, kShortCount, 0);
    descriptor(kVaLongCodes, kLongCount, 8);

    MdecCodebook book;
    for (const Word& word : words) book.maxLength = std::max(book.maxLength, word.length);
    book.lut.assign(static_cast<size_t>(1) << book.maxLength, 0u);
    const size_t symbols = ExeOffset(kVaDefaultSymbols);
    for (size_t i = 0; i < words.size(); ++i) {
        const size_t at = symbols + i * 2;
        if (at + 2 > exe.size()) throw std::runtime_error("chunk: EXE too short for the MDEC symbol table");
        const uint16_t symbol = static_cast<uint16_t>(exe[at] | (exe[at + 1] << 8));
        const int shift = book.maxLength - words[i].length;
        const size_t base = static_cast<size_t>(words[i].bits) << shift;
        const uint32_t entry = (static_cast<uint32_t>(words[i].length) << 16) | symbol;
        for (size_t n = 0; n < (static_cast<size_t>(1) << shift); ++n) book.lut[base + n] = entry;
    }
    return book;
}

Panorama ParsePanoramaChunk(std::span<const uint8_t> chunk, const MdecCodebook& book) {
    if (chunk.size() < kChunkSize) throw std::runtime_error("chunk: short chunk");
    if (book.Empty()) throw std::runtime_error("chunk: no MDEC codebook (read it out of the EXE first)");
    Panorama out;
    out.header = ParseChunkHeader(chunk);
    if (out.header.type != static_cast<uint8_t>(ChunkType::Panorama))
        throw std::runtime_error("chunk: not a panorama chunk");
    out.road = ReadU32(chunk, 0x24);
    out.distance = ReadU32(chunk, 0x28);

    // The sub-block chain. A tag is a big-endian ASCII u32, i.e. the four bytes spell it backwards.
    const auto tagAt = [&](size_t off, const char* tag) {
        return off + 4 <= chunk.size() && chunk[off] == static_cast<uint8_t>(tag[3]) &&
               chunk[off + 1] == static_cast<uint8_t>(tag[2]) &&
               chunk[off + 2] == static_cast<uint8_t>(tag[1]) && chunk[off + 3] == static_cast<uint8_t>(tag[0]);
    };
    size_t sten = 0, horz = 0, horzEnd = 0, at = 0x2C;
    std::vector<size_t> strips;
    while (at + 8 <= chunk.size()) {
        const uint32_t size = ReadU32(chunk, at + 4);
        if (size < 8 || at + size > chunk.size()) break;
        if (tagAt(at, "PANO")) {
            const uint32_t count = ReadU32(chunk, at + 0x1C);
            for (uint32_t i = 0; i < count && 0x20 + 4 * i + 4 <= size; ++i)
                out.refs.push_back(ReadU32(chunk, at + 0x20 + 4 * i));
        } else if (tagAt(at, "STEN")) {
            sten = at + 8;
        } else if (tagAt(at, "HORZ")) {
            horz = at;
            horzEnd = at + size;
        } else if (tagAt(at, "OFFS")) {
            // 112 bytes: one per column plus two of padding, read by
            // `RASHCDG 0x800644F4` at (ctx+0x3E0) + 8 + column.
            if (size >= 8 + static_cast<uint32_t>(Panorama::kColumns)) {
                for (int c = 0; c < Panorama::kColumns; ++c) out.offs[c] = chunk[at + 8 + static_cast<size_t>(c)];
                out.haveOffs = true;
            }
        } else if (tagAt(at, "MDEC")) {
            strips.push_back(at);
        }
        at += size;
    }
    if (sten == 0 || strips.size() != static_cast<size_t>(Panorama::kColumns) / 2)
        throw std::runtime_error("chunk: panorama has no STEN block or the wrong number of strips");

    // STEN: 880 two-bit codes, four per byte, MSB first, eight per column, the columns running on
    // through the payload without a break. Read exactly as `RASHCDG 0x800652E8` reads them - shift
    // 6, then 4, 2, 0, then the next byte - which is what makes the tile count come out right.
    for (int column = 0; column < Panorama::kColumns; ++column)
        for (int band = 0; band < Panorama::kBands; ++band) {
            const size_t index = static_cast<size_t>(column) * Panorama::kBands + static_cast<size_t>(band);
            const uint8_t packed = chunk[sten + index / 4];
            out.code[index] = static_cast<uint8_t>((packed >> (6 - 2 * (index % 4))) & 3u);
        }

    out.rgba.assign(static_cast<size_t>(Panorama::kWidth) * Panorama::kHeight * 4, 0);
    std::vector<uint8_t> rgb;
    for (size_t strip = 0; strip < strips.size(); ++strip) {
        const size_t block = strips[strip];
        const uint32_t width = ReadU32(chunk, block + 8);
        const uint32_t height = ReadU32(chunk, block + 0x0C);
        const uint32_t size = ReadU32(chunk, block + 4);
        if (height == 0) continue;
        const uint16_t magic = ReadU16(chunk, block + 0x12);
        const uint16_t qscale = ReadU16(chunk, block + 0x14);
        const uint16_t version = ReadU16(chunk, block + 0x16);
        if (magic != 0x3800 || version != 2 || width != static_cast<uint32_t>(Panorama::kTile))
            throw std::runtime_error("chunk: panorama strip is not a 16-pixel BS version 2 frame");
        DecodeBsFrame(book, chunk.data() + block + 0x18, size - 0x18, static_cast<int>(width),
                      static_cast<int>(height), qscale, rgb);

        // The strip holds the non-empty bands of column 2*strip followed by those of 2*strip+1.
        int tile = 0;
        for (int half = 0; half < 2; ++half) {
            const int column = static_cast<int>(strip) * 2 + half;
            for (int band = 0; band < Panorama::kBands; ++band) {
                if (out.code[static_cast<size_t>(column) * Panorama::kBands + static_cast<size_t>(band)] == 0)
                    continue;
                if (static_cast<uint32_t>(tile + 1) * Panorama::kTile > height)
                    throw std::runtime_error("chunk: panorama strip has fewer tiles than STEN asks for");
                for (int y = 0; y < Panorama::kTile; ++y)
                    for (int x = 0; x < Panorama::kTile; ++x) {
                        const size_t src = (static_cast<size_t>(tile * Panorama::kTile + y) * Panorama::kTile +
                                            static_cast<size_t>(x)) * 3;
                        const size_t dst = (static_cast<size_t>(band * Panorama::kTile + y) * Panorama::kWidth +
                                            static_cast<size_t>(column * Panorama::kTile + x)) * 4;
                        out.rgba[dst + 0] = rgb[src + 0];
                        out.rgba[dst + 1] = rgb[src + 1];
                        out.rgba[dst + 2] = rgb[src + 2];
                        out.rgba[dst + 3] = 255;
                    }
                ++tile;
                ++out.tiles;
            }
        }
        if (static_cast<uint32_t>(tile) * Panorama::kTile != height)
            throw std::runtime_error("chunk: panorama strip has more tiles than STEN asks for");
    }
    // HORZ: the skyline cut-outs. `RASHCDG 0x800657A8` hands every tile whose STEN code is 2 to
    // `0x800102A4` together with the next 8-byte record at HORZ + 0x48 (the records run in column
    // order, then top to bottom - `0x80065174` keeps the running count of code-2 tiles before each
    // column at ctx + 0x200). A record is 16 nibbles, one per decoded tile row y (high nibble of
    // byte y/2 for even y, low for odd); a non-zero n clears pixels 15 - n .. 15 of that row to
    // 0x0000, the GPU's transparent texel. The tile is drawn turned (screen x = row, screen y =
    // 15 - pixel), so each nibble cuts the TOP n + 1 pixels of one screen column: the sky above the
    // skyline shows through. Checked against the tile cache of `rr-race` (`rrview --skypacketcheck`).
    if (horz != 0) {
        size_t record = 0;
        for (int column = 0; column < Panorama::kColumns; ++column)
            for (int band = 0; band < Panorama::kBands; ++band) {
                if (out.code[static_cast<size_t>(column) * Panorama::kBands + static_cast<size_t>(band)] != 2) continue;
                const size_t at8 = horz + 0x48 + record * 8;
                ++record;
                if (at8 + 8 > horzEnd) throw std::runtime_error("chunk: panorama HORZ has fewer records than STEN asks for");
                for (int y = 0; y < Panorama::kTile; ++y) {
                    const uint8_t byte = chunk[at8 + static_cast<size_t>(y / 2)];
                    const int n = (y & 1) ? (byte & 0xF) : (byte >> 4);
                    if (n == 0) continue;
                    for (int x = Panorama::kTile - 1 - n; x < Panorama::kTile; ++x) {
                        const size_t dst = (static_cast<size_t>(band * Panorama::kTile + y) * Panorama::kWidth +
                                            static_cast<size_t>(column * Panorama::kTile + x)) * 4;
                        out.rgba[dst + 0] = out.rgba[dst + 1] = out.rgba[dst + 2] = out.rgba[dst + 3] = 0;
                    }
                }
            }
        out.horzRecords = static_cast<int>(record);
    }
    return out;
}

} // namespace rr
