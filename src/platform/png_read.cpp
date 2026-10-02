// The PNG decoder (png_read.h). Ported from the gt2-play project (MIT):
// src\gt2vfs\inflate.cpp (Inflate) and src\gt2formats\png_reader.cpp (DecodePng), unchanged but for the namespace.
#include "platform/png_read.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace rr {
namespace {

struct BitReader {
    std::span<const uint8_t> in;
    size_t pos = 0;
    uint32_t bitBuf = 0;
    int bitCount = 0;

    uint32_t Bits(int need) {
        uint32_t val = bitBuf;
        while (bitCount < need) {
            if (pos >= in.size()) throw std::runtime_error("inflate: unexpected end of input");
            val |= static_cast<uint32_t>(in[pos++]) << bitCount;
            bitCount += 8;
        }
        bitBuf = need == 32 ? 0 : val >> need;
        bitCount -= need;
        return need == 32 ? val : val & ((1u << need) - 1);
    }
};

constexpr int kMaxBits = 15;

struct Huffman {
    std::array<uint16_t, kMaxBits + 1> count{};
    std::array<uint16_t, 288> symbol{};

    void Build(const uint8_t* lengths, int n) {
        count.fill(0);
        for (int i = 0; i < n; i++) count[lengths[i]]++;
        int left = 1;
        for (int len = 1; len <= kMaxBits; len++) {
            left <<= 1;
            left -= count[len];
            if (left < 0) throw std::runtime_error("inflate: over-subscribed code");
        }
        std::array<uint16_t, kMaxBits + 1> offs{};
        for (int len = 1; len < kMaxBits; len++) offs[len + 1] = static_cast<uint16_t>(offs[len] + count[len]);
        for (int i = 0; i < n; i++)
            if (lengths[i] != 0) symbol[offs[lengths[i]]++] = static_cast<uint16_t>(i);
    }

    int Decode(BitReader& br) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= kMaxBits; len++) {
            code |= static_cast<int>(br.Bits(1));
            int c = count[len];
            if (code - c < first) return symbol[index + (code - first)];
            index += c;
            first += c;
            first <<= 1;
            code <<= 1;
        }
        throw std::runtime_error("inflate: invalid code");
    }
};

constexpr uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                   35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                   3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                    8193, 12289, 16385, 24577};
constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void InflateCodes(BitReader& br, std::vector<uint8_t>& out, const Huffman& lit, const Huffman& dist) {
    for (;;) {
        int sym = lit.Decode(br);
        if (sym < 256) {
            out.push_back(static_cast<uint8_t>(sym));
        } else if (sym == 256) {
            return;
        } else {
            sym -= 257;
            if (sym >= 29) throw std::runtime_error("inflate: bad length symbol");
            size_t len = kLenBase[sym] + br.Bits(kLenExtra[sym]);
            int ds = dist.Decode(br);
            if (ds >= 30) throw std::runtime_error("inflate: bad distance symbol");
            size_t d = kDistBase[ds] + br.Bits(kDistExtra[ds]);
            if (d > out.size()) throw std::runtime_error("inflate: distance too far back");
            size_t from = out.size() - d;
            for (size_t i = 0; i < len; i++) out.push_back(out[from + i]);
        }
    }
}

} // namespace

std::vector<uint8_t> Inflate(std::span<const uint8_t> deflate, size_t* consumed) {
    BitReader br{deflate};
    std::vector<uint8_t> out;
    out.reserve(deflate.size() * 3);

    bool last = false;
    while (!last) {
        last = br.Bits(1) != 0;
        uint32_t type = br.Bits(2);
        if (type == 0) {
            br.bitBuf = 0;
            br.bitCount = 0;
            if (br.pos + 4 > deflate.size()) throw std::runtime_error("inflate: truncated stored block");
            uint16_t len = static_cast<uint16_t>(deflate[br.pos] | (deflate[br.pos + 1] << 8));
            uint16_t nlen = static_cast<uint16_t>(deflate[br.pos + 2] | (deflate[br.pos + 3] << 8));
            br.pos += 4;
            if (len != static_cast<uint16_t>(~nlen)) throw std::runtime_error("inflate: stored length mismatch");
            if (br.pos + len > deflate.size()) throw std::runtime_error("inflate: truncated stored block");
            out.insert(out.end(), deflate.begin() + br.pos, deflate.begin() + br.pos + len);
            br.pos += len;
        } else if (type == 1) {
            uint8_t lengths[288];
            int i = 0;
            for (; i < 144; i++) lengths[i] = 8;
            for (; i < 256; i++) lengths[i] = 9;
            for (; i < 280; i++) lengths[i] = 7;
            for (; i < 288; i++) lengths[i] = 8;
            Huffman lit, dist;
            lit.Build(lengths, 288);
            uint8_t dl[30];
            for (auto& v : dl) v = 5;
            dist.Build(dl, 30);
            InflateCodes(br, out, lit, dist);
        } else if (type == 2) {
            int nlen = static_cast<int>(br.Bits(5)) + 257;
            int ndist = static_cast<int>(br.Bits(5)) + 1;
            int ncode = static_cast<int>(br.Bits(4)) + 4;
            if (nlen > 286 || ndist > 30) throw std::runtime_error("inflate: bad counts");
            static constexpr uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            uint8_t lengths[320] = {};
            for (int i = 0; i < ncode; i++) lengths[order[i]] = static_cast<uint8_t>(br.Bits(3));
            Huffman lenCode;
            lenCode.Build(lengths, 19);
            int index = 0;
            uint8_t all[320] = {};
            while (index < nlen + ndist) {
                int sym = lenCode.Decode(br);
                if (sym < 16) {
                    all[index++] = static_cast<uint8_t>(sym);
                } else {
                    uint8_t prev = 0;
                    int rep;
                    if (sym == 16) {
                        if (index == 0) throw std::runtime_error("inflate: repeat without previous length");
                        prev = all[index - 1];
                        rep = 3 + static_cast<int>(br.Bits(2));
                    } else if (sym == 17) {
                        rep = 3 + static_cast<int>(br.Bits(3));
                    } else {
                        rep = 11 + static_cast<int>(br.Bits(7));
                    }
                    if (index + rep > nlen + ndist) throw std::runtime_error("inflate: too many lengths");
                    while (rep--) all[index++] = prev;
                }
            }
            Huffman lit, dist;
            lit.Build(all, nlen);
            dist.Build(all + nlen, ndist);
            InflateCodes(br, out, lit, dist);
        } else {
            throw std::runtime_error("inflate: invalid block type");
        }
    }
    if (consumed) *consumed = br.pos;
    return out;
}

namespace {

uint32_t U32Be(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

int Paeth(int a, int b, int c) {
    const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

} // namespace

PngImage DecodePng(std::span<const uint8_t> file) {
    static const uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (file.size() < 8 || std::memcmp(file.data(), kSignature, 8) != 0) throw std::runtime_error("png: bad signature");

    uint32_t width = 0, height = 0;
    int bitDepth = 0, colorType = 0, interlace = 0;
    bool haveHeader = false;
    std::vector<uint8_t> palette, trns, zlib;
    for (size_t pos = 8; pos + 8 <= file.size();) {
        const uint32_t length = U32Be(&file[pos]);
        if (pos + 12 + size_t(length) > file.size()) throw std::runtime_error("png: truncated chunk");
        const std::span<const uint8_t> data = file.subspan(pos + 8, length);
        const char* type = reinterpret_cast<const char*>(&file[pos + 4]);
        if (std::memcmp(type, "IHDR", 4) == 0) {
            if (length != 13) throw std::runtime_error("png: bad IHDR");
            width = U32Be(&data[0]);
            height = U32Be(&data[4]);
            bitDepth = data[8];
            colorType = data[9];
            interlace = data[12];
            haveHeader = true;
        } else if (std::memcmp(type, "PLTE", 4) == 0) {
            palette.assign(data.begin(), data.end());
        } else if (std::memcmp(type, "tRNS", 4) == 0) {
            trns.assign(data.begin(), data.end());
        } else if (std::memcmp(type, "IDAT", 4) == 0) {
            zlib.insert(zlib.end(), data.begin(), data.end());
        } else if (std::memcmp(type, "IEND", 4) == 0) {
            break;
        }
        pos += 12 + size_t(length);
    }
    if (!haveHeader) throw std::runtime_error("png: no IHDR");
    if (interlace != 0) throw std::runtime_error("png: interlaced images are not supported");
    if (width == 0 || height == 0 || width > 16384 || height > 16384) throw std::runtime_error("png: unsupported image size");
    int channels;
    switch (colorType) {
    case 0: channels = 1; break; // grey
    case 2: channels = 3; break; // RGB
    case 3: channels = 1; break; // palette
    case 4: channels = 2; break; // grey + alpha
    case 6: channels = 4; break; // RGBA
    default: throw std::runtime_error("png: bad colour type");
    }
    const bool depthOk = colorType == 3 ? (bitDepth == 1 || bitDepth == 2 || bitDepth == 4 || bitDepth == 8)
                         : colorType == 0 ? (bitDepth == 1 || bitDepth == 2 || bitDepth == 4 || bitDepth == 8 || bitDepth == 16)
                                          : (bitDepth == 8 || bitDepth == 16);
    if (!depthOk) throw std::runtime_error("png: bad bit depth");
    if (colorType == 3 && palette.empty()) throw std::runtime_error("png: palette image without PLTE");
    if (zlib.size() < 6) throw std::runtime_error("png: no image data");

    // zlib wrapper: 2-byte header, raw deflate, adler32 (not verified).
    if ((zlib[0] & 0x0F) != 8 || ((zlib[0] << 8) | zlib[1]) % 31 != 0) throw std::runtime_error("png: bad zlib header");
    const std::vector<uint8_t> raw = Inflate(std::span<const uint8_t>(zlib).subspan(2, zlib.size() - 6));

    const size_t bitsPerPixel = size_t(channels) * size_t(bitDepth);
    const size_t stride = (size_t(width) * bitsPerPixel + 7) / 8;
    const size_t bpp = bitsPerPixel < 8 ? 1 : bitsPerPixel / 8; // filter byte distance
    if (raw.size() < (stride + 1) * height) throw std::runtime_error("png: image data too short");

    std::vector<uint8_t> scan(stride * height);
    std::vector<uint8_t> previous(stride, 0);
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t filter = raw[y * (stride + 1)];
        const uint8_t* src = &raw[y * (stride + 1) + 1];
        uint8_t* dst = &scan[y * stride];
        for (size_t x = 0; x < stride; x++) {
            const int a = x >= bpp ? dst[x - bpp] : 0, b = previous[x], c = x >= bpp ? previous[x - bpp] : 0;
            int v;
            switch (filter) {
            case 0: v = src[x]; break;
            case 1: v = src[x] + a; break;
            case 2: v = src[x] + b; break;
            case 3: v = src[x] + ((a + b) >> 1); break;
            case 4: v = src[x] + Paeth(a, b, c); break;
            default: throw std::runtime_error("png: bad filter type");
            }
            dst[x] = uint8_t(v);
        }
        std::memcpy(previous.data(), dst, stride);
    }

    PngImage image;
    image.width = int(width);
    image.height = int(height);
    image.rgba.resize(size_t(width) * height * 4);
    // Sample fetch: value scaled to 8 bits (16-bit samples keep the high byte; sub-byte samples are stretched).
    auto sample = [&](const uint8_t* row, size_t index) -> int {
        if (bitDepth == 8) return row[index];
        if (bitDepth == 16) return row[index * 2];
        const size_t bit = index * size_t(bitDepth);
        const int v = (row[bit / 8] >> (8 - bitDepth - int(bit % 8))) & ((1 << bitDepth) - 1);
        return colorType == 3 ? v : v * 255 / ((1 << bitDepth) - 1);
    };
    // Full-precision sample for the tRNS colour-key comparison.
    auto sampleRaw = [&](const uint8_t* row, size_t index) -> int {
        if (bitDepth == 16) return (row[index * 2] << 8) | row[index * 2 + 1];
        if (bitDepth == 8) return row[index];
        const size_t bit = index * size_t(bitDepth);
        return (row[bit / 8] >> (8 - bitDepth - int(bit % 8))) & ((1 << bitDepth) - 1);
    };
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* row = &scan[y * stride];
        for (uint32_t x = 0; x < width; x++) {
            uint8_t* out = &image.rgba[(size_t(y) * width + x) * 4];
            int r, g, b, a = 255;
            switch (colorType) {
            case 0: {
                r = g = b = sample(row, x);
                if (trns.size() >= 2 && sampleRaw(row, x) == ((trns[0] << 8) | trns[1])) a = 0;
                break;
            }
            case 2: {
                r = sample(row, x * 3);
                g = sample(row, x * 3 + 1);
                b = sample(row, x * 3 + 2);
                if (trns.size() >= 6 && sampleRaw(row, x * 3) == ((trns[0] << 8) | trns[1]) && sampleRaw(row, x * 3 + 1) == ((trns[2] << 8) | trns[3]) &&
                    sampleRaw(row, x * 3 + 2) == ((trns[4] << 8) | trns[5]))
                    a = 0;
                break;
            }
            case 3: {
                const size_t index = size_t(sample(row, x));
                if (index * 3 + 2 >= palette.size()) throw std::runtime_error("png: palette index out of range");
                r = palette[index * 3];
                g = palette[index * 3 + 1];
                b = palette[index * 3 + 2];
                if (index < trns.size()) a = trns[index];
                break;
            }
            case 4: {
                r = g = b = sample(row, x * 2);
                a = sample(row, x * 2 + 1);
                break;
            }
            default: {
                r = sample(row, x * 4);
                g = sample(row, x * 4 + 1);
                b = sample(row, x * 4 + 2);
                a = sample(row, x * 4 + 3);
                break;
            }
            }
            out[0] = uint8_t(r);
            out[1] = uint8_t(g);
            out[2] = uint8_t(b);
            out[3] = uint8_t(a);
        }
    }
    return image;
}

} // namespace rr
