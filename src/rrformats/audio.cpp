#include "rrformats/audio.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace rr {
namespace {

uint32_t ReadU32Be(std::span<const uint8_t> d, size_t off) {
    if (off + 4 > d.size()) throw std::runtime_error("audio: read past end of file");
    return (static_cast<uint32_t>(d[off]) << 24) | (static_cast<uint32_t>(d[off + 1]) << 16) |
           (static_cast<uint32_t>(d[off + 2]) << 8) | static_cast<uint32_t>(d[off + 3]);
}

uint32_t ReadU32Le(std::span<const uint8_t> d, size_t off) {
    if (off + 4 > d.size()) throw std::runtime_error("audio: read past end of file");
    return static_cast<uint32_t>(d[off]) | (static_cast<uint32_t>(d[off + 1]) << 8) |
           (static_cast<uint32_t>(d[off + 2]) << 16) | (static_cast<uint32_t>(d[off + 3]) << 24);
}

uint16_t ReadU16Le(std::span<const uint8_t> d, size_t off) {
    if (off + 2 > d.size()) throw std::runtime_error("audio: read past end of file");
    return static_cast<uint16_t>(d[off] | (d[off + 1] << 8));
}

std::string UpperCase(std::string s) {
    for (char& c : s) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return s;
}

bool EndsWith(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

std::string BaseName(const std::string& path) {
    const size_t cut = path.find_last_of("\\/");
    return cut == std::string::npos ? path : path.substr(cut + 1);
}

} // namespace

// ------------------------------------------------------------------------------------------------
// SPU-ADPCM
// ------------------------------------------------------------------------------------------------

void DecodeAdpcm(std::span<const uint8_t> data, AdpcmBlock block, AdpcmState& state,
                 std::vector<int16_t>& out) {
    const size_t blockSize = block == AdpcmBlock::Spu16 ? kSpuBlockSize : kWveBlockSize;
    const size_t headerSize = block == AdpcmBlock::Spu16 ? 2u : 1u;
    if (data.size() < blockSize) return;
    const size_t blocks = data.size() / blockSize;
    out.reserve(out.size() + blocks * kAdpcmSamplesPerBlock);

    int32_t prev1 = state.prev1;
    int32_t prev2 = state.prev2;
    for (size_t b = 0; b < blocks; ++b) {
        const size_t base = b * blockSize;
        const uint8_t header = data[base];
        int shift = header & 0x0F;
        int filter = header >> 4;
        if (filter > 4) {
            if (!state.lenient)
                throw std::runtime_error("audio: ADPCM filter index " + std::to_string(filter) +
                                         " > 4 in block at 0x" + std::to_string(base) +
                                         " - the block grid is wrong");
            ++state.badFilter;
            filter = 0;
        }
        if (shift > 12) {
            if (!state.lenient)
                throw std::runtime_error("audio: ADPCM shift " + std::to_string(shift) +
                                         " > 12 in block at 0x" + std::to_string(base) +
                                         " - the block grid is wrong");
            ++state.badShift;
            shift = 9;
        }
        const int f0 = kSpuFilterF0[filter];
        const int f1 = kSpuFilterF1[filter];
        for (size_t i = base + headerSize; i < base + blockSize; ++i) {
            const uint8_t byte = data[i];
            for (int half = 0; half < 2; ++half) {
                const int nibble = half == 0 ? (byte & 0x0F) : (byte >> 4);
                const int s = (nibble & 8) ? nibble - 16 : nibble;
                // (s << 12) >> shift, written as a multiply so a negative s never relies on the
                // shift-of-negative rule; the predictor term rounds with +32 before the /64.
                int32_t v = ((s * 4096) >> shift) + ((prev1 * f0 + prev2 * f1 + 32) >> 6);
                if (v > 32767) {
                    v = 32767;
                    ++state.clipped;
                } else if (v < -32768) {
                    v = -32768;
                    ++state.clipped;
                }
                out.push_back(static_cast<int16_t>(v));
                prev2 = prev1;
                prev1 = v;
            }
        }
    }
    state.prev1 = prev1;
    state.prev2 = prev2;
}

// ------------------------------------------------------------------------------------------------
// .WVE
// ------------------------------------------------------------------------------------------------

namespace {

struct RawChunk {
    size_t offset;
    char tag[4];
    uint32_t size;
};

std::vector<RawChunk> WalkChunks(std::span<const uint8_t> file) {
    std::vector<RawChunk> chunks;
    size_t off = 0;
    while (off + 8 <= file.size()) {
        RawChunk c{};
        c.offset = off;
        std::memcpy(c.tag, file.data() + off, 4);
        c.size = ReadU32Be(file, off + 4);
        if (c.size < 8 || off + c.size > file.size())
            throw std::runtime_error("audio: bad chunk at 0x" + std::to_string(off) + " size " +
                                     std::to_string(c.size));
        chunks.push_back(c);
        off += c.size;
    }
    if (off != file.size())
        throw std::runtime_error("audio: chunk chain does not close on the file size");
    return chunks;
}

} // namespace

std::vector<WveAudioChunk> WveAudioChunks(std::span<const uint8_t> file) {
    std::vector<WveAudioChunk> out;
    for (const RawChunk& c : WalkChunks(file)) {
        if (c.tag[0] != 'a' || c.tag[1] != 'u') continue;
        if (c.size < 16)
            throw std::runtime_error("audio: au chunk at 0x" + std::to_string(c.offset) +
                                     " is too small for its 16-byte header");
        WveAudioChunk a;
        a.offset = c.offset;
        a.tag.assign(c.tag, c.tag + 4);
        a.size = c.size;
        a.firstSample = ReadU32Be(file, c.offset + 8);
        a.field0C = ReadU32Be(file, c.offset + 12);
        a.payloadOffset = c.offset + 16;
        a.payloadSize = c.size - 16;
        a.blocks = a.payloadSize / kWveBlockSize;
        a.halfBlocks = a.blocks / 2;
        out.push_back(std::move(a));
    }
    return out;
}

PcmBuffer DecodeWve(std::span<const uint8_t> file, WveAudioInfo* info) {
    WveAudioInfo local;
    for (const RawChunk& c : WalkChunks(file))
        if (std::memcmp(c.tag, "MDEC", 4) == 0) ++local.videoFrames;

    const std::vector<WveAudioChunk> chunks = WveAudioChunks(file);
    std::vector<int16_t> left, right;
    bool haveExpected = false;
    uint32_t expected = 0;
    for (const WveAudioChunk& c : chunks) {
        if (haveExpected && c.firstSample != expected) ++local.counterMismatch;
        expected = c.firstSample + static_cast<uint32_t>(c.halfBlocks * kAdpcmSamplesPerBlock);
        haveExpected = true;
        const size_t half = c.halfBlocks * kWveBlockSize;
        // Planar inside the chunk: N blocks of left, then N of right. Anything past 2N blocks is the
        // 0 or 2 bytes of padding that round the chunk size up to a multiple of 4.
        DecodeAdpcm(file.subspan(c.payloadOffset, half), AdpcmBlock::Wve15, local.left, left);
        DecodeAdpcm(file.subspan(c.payloadOffset + half, half), AdpcmBlock::Wve15, local.right, right);
        ++local.chunks;
    }

    PcmBuffer pcm;
    pcm.channels = 2;
    pcm.sampleRate = kWveSampleRate;
    const size_t frames = std::min(left.size(), right.size());
    pcm.samples.resize(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        pcm.samples[i * 2] = left[i];
        pcm.samples[i * 2 + 1] = right[i];
    }
    if (info) *info = local;
    return pcm;
}

// ------------------------------------------------------------------------------------------------
// .ALB
// ------------------------------------------------------------------------------------------------

size_t AlbUnitCount(size_t fileSize) { return fileSize / kAlbUnitSize; }

void DecodeAlbUnit(std::span<const uint8_t> unit, AdpcmState& left, AdpcmState& right,
                   std::vector<int16_t>& out) {
    if (unit.size() != kAlbUnitSize)
        throw std::runtime_error("audio: .ALB unit must be exactly 16384 bytes, got " +
                                 std::to_string(unit.size()));
    std::vector<int16_t> l, r;
    l.reserve(kAlbSamplesPerUnit);
    r.reserve(kAlbSamplesPerUnit);
    DecodeAdpcm(unit.subspan(0, kAlbHalfSize), AdpcmBlock::Spu16, left, l);
    DecodeAdpcm(unit.subspan(kAlbHalfSize, kAlbHalfSize), AdpcmBlock::Spu16, right, r);
    const size_t frames = std::min(l.size(), r.size());
    out.reserve(out.size() + frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        out.push_back(l[i]);
        out.push_back(r[i]);
    }
}

PcmBuffer DecodeAlb(std::span<const uint8_t> file, size_t startUnit, size_t units,
                    AdpcmState* leftOut, AdpcmState* rightOut) {
    // Every .ALB on the disc is an exact multiple of the 16 KiB stereo unit; anything else is not one.
    if (file.empty() || file.size() % kAlbUnitSize != 0)
        throw std::runtime_error("audio: " + std::to_string(file.size()) +
                                 " bytes is not a whole number of 16 KiB .ALB units");
    const size_t total = AlbUnitCount(file.size());
    if (startUnit > total)
        throw std::runtime_error("audio: .ALB start unit " + std::to_string(startUnit) +
                                 " is past the end (" + std::to_string(total) + " units)");
    const size_t count = units == 0 ? total - startUnit : std::min(units, total - startUnit);

    AdpcmState left, right;
    PcmBuffer pcm;
    pcm.channels = 2;
    pcm.sampleRate = kAlbSampleRate;
    pcm.samples.reserve(count * kAlbSamplesPerUnit * 2);
    for (size_t u = 0; u < count; ++u)
        DecodeAlbUnit(file.subspan((startUnit + u) * kAlbUnitSize, kAlbUnitSize), left, right,
                      pcm.samples);
    if (leftOut) *leftOut = left;
    if (rightOut) *rightOut = right;
    return pcm;
}

std::vector<size_t> AlbLoopEndBlocks(std::span<const uint8_t> file) {
    std::vector<size_t> out;
    for (size_t off = 0; off + kSpuBlockSize <= file.size(); off += kSpuBlockSize)
        if (file[off + 1] & 1) out.push_back(off);
    return out;
}

std::vector<AlbTrack> ParseAlbumTrackTable(std::span<const uint8_t> exe) {
    const size_t base = kAlbumTrackTableFileOffset;
    if (base + 8 > exe.size())
        throw std::runtime_error("audio: the EXE is too short to hold the album track table");
    const uint32_t count = ReadU32Le(exe, base);
    const uint32_t total = ReadU32Le(exe, base + 4);
    if (count == 0 || count > 256)
        throw std::runtime_error("audio: album track count " + std::to_string(count) +
                                 " is not plausible - wrong EXE?");
    if (base + 8 + static_cast<size_t>(count) * 12 > exe.size())
        throw std::runtime_error("audio: the album track table runs past the end of the EXE");

    std::vector<AlbTrack> tracks;
    tracks.reserve(count);
    uint64_t expected = 0;
    for (uint32_t i = 0; i < count; ++i) {
        AlbTrack t;
        t.index = i;
        t.start = ReadU32Le(exe, base + 8 + i * 12);
        t.length = ReadU32Le(exe, base + 12 + i * 12);
        t.flags = ReadU32Le(exe, base + 16 + i * 12);
        // The three invariants that make this table self-proving rather than a guessed offset.
        if (t.start != expected)
            throw std::runtime_error("audio: album track " + std::to_string(i) +
                                     " does not start where the previous one ends");
        if (t.length == 0 || t.length % kAlbUnitSize != 0)
            throw std::runtime_error("audio: album track " + std::to_string(i) +
                                     " is not a whole number of 16 KiB units");
        expected += t.length;
        tracks.push_back(t);
    }
    if (expected != total)
        throw std::runtime_error("audio: album track lengths sum to " + std::to_string(expected) +
                                 " but the table's total is " + std::to_string(total));
    return tracks;
}

// ------------------------------------------------------------------------------------------------
// AUDTAUNT.STR
// ------------------------------------------------------------------------------------------------

namespace {

constexpr uint8_t kSpuFlagLoopEnd = 0x01;
constexpr uint8_t kSpuFlagRepeat = 0x02;
constexpr uint8_t kSpuFlagLoopStart = 0x04;

// Walks one sample from `start` (a file offset) to its loop-end block, and checks that the dummy
// terminator block follows it. `limit` is the first byte past the record body.
TauntSample ScanTauntSample(std::span<const uint8_t> file, size_t start, size_t limit, size_t record) {
    const std::string where = "audio: AUDTAUNT record " + std::to_string(record) + " sample at 0x" +
                              std::to_string(start);
    if (start + kSpuBlockSize > limit) throw std::runtime_error(where + ": starts past the body");
    if (!(file[start + 1] & kSpuFlagLoopStart))
        throw std::runtime_error(where + ": first block does not carry the loop-start flag");
    for (size_t off = start; off + kSpuBlockSize <= limit; off += kSpuBlockSize) {
        const uint8_t flags = file[off + 1];
        if (!(flags & kSpuFlagLoopEnd)) continue;
        // The last audio block is flagged loop-end without repeat; one dummy block carrying
        // loop-start|repeat|loop-end closes the sample so the SPU voice has somewhere to land.
        if (flags & kSpuFlagRepeat)
            throw std::runtime_error(where + ": hit a repeat-flagged block before the loop end");
        const size_t terminator = off + kSpuBlockSize;
        if (terminator + kSpuBlockSize > limit ||
            file[terminator + 1] != (kSpuFlagLoopEnd | kSpuFlagRepeat | kSpuFlagLoopStart))
            throw std::runtime_error(where + ": no terminator block after the loop end");
        TauntSample s;
        s.offset = start;
        s.blocks = (terminator - start) / kSpuBlockSize;
        s.size = s.blocks * kSpuBlockSize;
        s.terminatorOffset = terminator;
        return s;
    }
    throw std::runtime_error(where + ": no loop-end block inside the record");
}

} // namespace

std::vector<TauntRecord> TauntRecords(std::span<const uint8_t> file) {
    if (file.size() % kTauntRecordSize != 0)
        throw std::runtime_error("audio: AUDTAUNT.STR is not a whole number of 0x4000 records");
    std::vector<TauntRecord> out;
    const size_t count = file.size() / kTauntRecordSize;
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const size_t off = i * kTauntRecordSize;
        TauntRecord r;
        r.index = i;
        r.offset = off;
        r.soundId = file[off];
        r.voiceId = file[off + 1];
        r.id20 = ReadU32Le(file, off + 0x20);
        r.id24 = ReadU32Le(file, off + 0x24);
        r.volume = ReadU16Le(file, off + 0x34);
        r.sampleCount = file[off + 0x2C];
        r.descriptorListOffset = ReadU32Le(file, off + 0x38);
        r.secondSampleOffset = ReadU32Le(file, off + 0x5C);
        r.dataOffset = off + kTauntHeaderSize;
        const size_t limit = off + kTauntRecordSize;
        const std::string what = "audio: AUDTAUNT record " + std::to_string(i);
        // The count is stored three times followed by 0x7F; if that does not hold we are not looking
        // at this record layout at all.
        if (r.sampleCount < 1 || r.sampleCount > 2 || file[off + 0x2D] != r.sampleCount ||
            file[off + 0x2E] != r.sampleCount || file[off + 0x2F] != 0x7F)
            throw std::runtime_error(what + ": the sample count at +0x2C is not 1 or 2 in triplicate");
        if ((r.sampleCount == 2) != (r.secondSampleOffset != 0))
            throw std::runtime_error(what + ": the sample count at +0x2C disagrees with the second "
                                            "sample offset at +0x5C");
        if (r.secondSampleOffset > kTauntRecordSize - kTauntHeaderSize ||
            r.secondSampleOffset % kSpuBlockSize != 0)
            throw std::runtime_error(what + " field +0x5C = " + std::to_string(r.secondSampleOffset) +
                                     " is not a block-aligned offset inside the record body");

        // The descriptor list is NOT at a fixed offset: +0x38 says where it starts, relative to
        // +0x28 (20 with one sample, 24 with two). Each entry is 16 bytes - a 4-byte prefix and then
        // the 12-byte engine sound descriptor { u8 vol; u8; u16 adsr1; u16 adsr2; u16 pitch; u32 addr }
        // whose pitch halfword therefore sits 10 bytes into the entry.
        const size_t list = off + 0x28 + r.descriptorListOffset;
        if (r.descriptorListOffset > kTauntHeaderSize ||
            list + 16u * r.sampleCount > off + kTauntHeaderSize)
            throw std::runtime_error(what + ": the descriptor list at +0x38 does not fit in the header");

        TauntSample first = ScanTauntSample(file, r.dataOffset, limit, i);
        first.id = r.id20;
        first.pitch = ReadU16Le(file, list + 10);
        first.sampleRate = SpuPitchToRate(first.pitch);
        r.samples.push_back(first);
        if (r.secondSampleOffset) {
            TauntSample second =
                ScanTauntSample(file, r.dataOffset + r.secondSampleOffset, limit, i);
            second.id = r.id24;
            second.pitch = ReadU16Le(file, list + 16 + 10);
            second.sampleRate = SpuPitchToRate(second.pitch);
            r.samples.push_back(second);
        }
        for (const TauntSample& s : r.samples)
            if (s.pitch == 0)
                throw std::runtime_error(what + ": a sample descriptor carries pitch 0");
        out.push_back(std::move(r));
    }
    return out;
}

PcmBuffer DecodeTaunt(std::span<const uint8_t> file, const TauntSample& sample,
                      AdpcmState* stateOut) {
    PcmBuffer pcm;
    pcm.channels = 1;
    pcm.sampleRate = sample.sampleRate;
    AdpcmState state;
    if (sample.size)
        DecodeAdpcm(file.subspan(sample.offset, sample.size), AdpcmBlock::Spu16, state, pcm.samples);
    if (stateOut) *stateOut = state;
    return pcm;
}

PcmBuffer DecodeTauntProbeSpan(std::span<const uint8_t> file, const TauntRecord& record,
                               AdpcmState* stateOut) {
    PcmBuffer pcm;
    pcm.channels = 1;
    pcm.sampleRate = 22050; // what the probe writes, so the two WAVs can be compared byte for byte
    AdpcmState state;
    if (record.secondSampleOffset)
        DecodeAdpcm(file.subspan(record.dataOffset, record.secondSampleOffset), AdpcmBlock::Spu16,
                    state, pcm.samples);
    if (stateOut) *stateOut = state;
    return pcm;
}

// ------------------------------------------------------------------------------------------------

AudioFamily DetectAudioFamily(const std::string& name, std::span<const uint8_t> file) {
    if (file.size() >= 4 && std::memcmp(file.data(), "VLC0", 4) == 0) return AudioFamily::Wve;
    const std::string upper = UpperCase(BaseName(name));
    if (EndsWith(upper, ".ALB")) return AudioFamily::Alb;
    if (upper == "AUDTAUNT.STR") return AudioFamily::Taunt;
    return AudioFamily::Unknown;
}

const char* AudioFamilyName(AudioFamily family) {
    switch (family) {
        case AudioFamily::Wve: return "wve";
        case AudioFamily::Alb: return "alb";
        case AudioFamily::Taunt: return "taunt";
        default: return "unknown";
    }
}

} // namespace rr
