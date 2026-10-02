#pragma once
// Audio containers of Road Rash: Jailbreak (USA, SLUS_01053).
//
// Every sound on the disc is PS1 SPU-ADPCM - there is no CD-XA stream on it at all. Three families,
// one codec, three block/interleave shapes; layout and evidence in docs\formats\audio.md. This is the
// authoritative parser (the Python probe tools\scout\audio.py stays as an independent cross-check).
//
//   DATA\FE\*.WVE   "au00"/"au01" chunks of the VLC0 container: 15-byte blocks (the SPU flags byte is
//                   dropped), planar stereo inside one chunk, predictor state carried across chunks.
//   DATA\*.ALB      raw 16-byte SPU blocks, stereo planar in 8 KiB halves of a 16 KiB unit.
//   DATA\AUDTAUNT.STR  150 records of 0x4000 bytes: 0x70-byte header + mono 16-byte SPU blocks.
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace rr {

constexpr int kAdpcmSamplesPerBlock = 28;
constexpr size_t kSpuBlockSize = 16;  // shift/filter byte, flags byte, 14 data bytes
constexpr size_t kWveBlockSize = 15;  // shift/filter byte, 14 data bytes - no flags byte

// The SPU's pitch register: 0x1000 plays a sample at 44100 Hz, and the playback rate is linear in it.
// Every rate below comes from a pitch value the game actually programs, not from a guess.
constexpr int kSpuPitchUnit = 0x1000;
constexpr int kSpuBaseRate = 44100;
constexpr int SpuPitchToRate(int pitch) {
    return (kSpuBaseRate * pitch + kSpuPitchUnit / 2) / kSpuPitchUnit;
}

constexpr int kWvePitch = 0x0800;               // 22050 Hz exactly
constexpr int kAlbPitch = 0x05CE;               // 15999 Hz (the game asks for 16000; the SPU quantises)
constexpr int kWveSampleRate = SpuPitchToRate(kWvePitch);
constexpr int kAlbSampleRate = SpuPitchToRate(kAlbPitch);

// The five SPU predictor filters, as (f0, f1) with 64 = 1.0.
constexpr int kSpuFilterF0[5] = {0, 60, 115, 98, 122};
constexpr int kSpuFilterF1[5] = {0, 0, -52, -55, -60};

enum class AdpcmBlock {
    Spu16, // header, flags, 14 data
    Wve15, // header, 14 data
};

// Predictor state plus the counters that make a decode auditable. The state has to survive from one
// block to the next AND from one container chunk to the next, per channel.
struct AdpcmState {
    int32_t prev1 = 0;
    int32_t prev2 = 0;
    uint64_t clipped = 0;    // samples that hit the int16 rail
    uint64_t badFilter = 0;  // header nibble > 4   (only counted when lenient)
    uint64_t badShift = 0;   // header nibble > 12  (only counted when lenient)
    // Strict by default: an impossible header byte means the block grid is wrong, which is a bug in the
    // caller, not something to paper over. `lenient` reproduces the probe's substitution (filter 0 /
    // shift 9) and only counts, so a survey pass can report how much of a file is undecodable.
    bool lenient = false;
};

// Decodes floor(data.size() / block) blocks, appending 28 samples each. A trailing partial block is
// ignored (containers pad chunks, and the padding is never a block).
void DecodeAdpcm(std::span<const uint8_t> data, AdpcmBlock block, AdpcmState& state,
                 std::vector<int16_t>& out);

// Interleaved 16-bit PCM.
struct PcmBuffer {
    int channels = 0;
    int sampleRate = kWveSampleRate;
    std::vector<int16_t> samples; // channels * frames, interleaved
    size_t Frames() const { return channels ? samples.size() / static_cast<size_t>(channels) : 0; }
    double Seconds() const { return sampleRate ? static_cast<double>(Frames()) / sampleRate : 0.0; }
};

// ---------------------------------------------------------------------------------------------
// .WVE - "au00" / "au01" chunks
// ---------------------------------------------------------------------------------------------

struct WveAudioChunk {
    size_t offset = 0;      // of the chunk header inside the file
    std::string tag;        // "au00" or "au01"
    uint32_t size = 0;      // incl. the 16-byte header
    uint32_t firstSample = 0; // +0x08: sample index of this chunk's first sample, per channel
    uint32_t field0C = 0;     // +0x0C: 0x00020008 on all 1339 audio chunks of this disc; unknown
    size_t payloadOffset = 0;
    size_t payloadSize = 0;
    size_t blocks = 0;      // payloadSize / 15
    size_t halfBlocks = 0;  // blocks / 2, i.e. blocks per channel
};

// Walks the whole VLC0 chunk chain and returns the audio chunks. Throws if the chain does not close.
std::vector<WveAudioChunk> WveAudioChunks(std::span<const uint8_t> file);

struct WveAudioInfo {
    size_t chunks = 0;
    size_t videoFrames = 0;   // "MDEC" chunks, for the samples-per-frame ratio
    uint32_t counterMismatch = 0; // chunks whose +0x08 counter is not previous + halfBlocks*28
    AdpcmState left, right;
};

// Decodes every audio chunk of a .WVE, carrying the predictor state across chunks per channel.
PcmBuffer DecodeWve(std::span<const uint8_t> file, WveAudioInfo* info = nullptr);

// ---------------------------------------------------------------------------------------------
// .ALB - streamed music banks
// ---------------------------------------------------------------------------------------------

constexpr size_t kAlbHalfSize = 8192;                 // one channel's run
constexpr size_t kAlbUnitSize = kAlbHalfSize * 2;     // 16 KiB stereo unit
constexpr size_t kAlbSamplesPerUnit =
    (kAlbHalfSize / kSpuBlockSize) * static_cast<size_t>(kAdpcmSamplesPerBlock); // 14336 per channel

size_t AlbUnitCount(size_t fileSize);

// One 16 KiB unit -> `unit` must be exactly kAlbUnitSize bytes. Appends interleaved stereo frames.
void DecodeAlbUnit(std::span<const uint8_t> unit, AdpcmState& left, AdpcmState& right,
                   std::vector<int16_t>& out);

// `units == 0` means "to the end of the file".
PcmBuffer DecodeAlb(std::span<const uint8_t> file, size_t startUnit = 0, size_t units = 0,
                    AdpcmState* leftOut = nullptr, AdpcmState* rightOut = nullptr);

// Positions of the blocks carrying the SPU loop-end flag (bit 0 of the flags byte). FEALBUM.ALB uses
// them as bank delimiters, ALBUM.ALB and INTRO.ALB have none.
std::vector<size_t> AlbLoopEndBlocks(std::span<const uint8_t> file);

// --- ALBUM.ALB track table -------------------------------------------------------------------
//
// It is not in the .ALB: it lives in the main EXE, at vaddr 0x80053578 = file offset 0x43D78 of
// `SLUS_010.53` (SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1), as
//   u32 trackCount; u32 totalBytes; { u32 start; u32 length; u32 flags; }[trackCount]
// `flags` bit0 = track selectable, bit1 = already played in this shuffle cycle.
// ParseAlbumTrackTable checks the invariants that make this self-proving: the entries are contiguous
// from 0, every length is a whole number of 16 KiB units, and the total equals ALBUM.ALB's size.
constexpr size_t kAlbumTrackTableFileOffset = 0x43D78;

struct AlbTrack {
    size_t index = 0;
    uint32_t start = 0;   // byte offset into ALBUM.ALB
    uint32_t length = 0;  // bytes
    uint32_t flags = 0;
    size_t StartUnit() const { return start / kAlbUnitSize; }
    size_t Units() const { return length / kAlbUnitSize; }
};

// `exe` is the whole `SLUS_010.53` file. Throws when the invariants do not hold.
std::vector<AlbTrack> ParseAlbumTrackTable(std::span<const uint8_t> exe);

// FEALBUM.ALB needs no table: RASHCDF.BIN computes the offset of frontend track n as n * 0x70000,
// and 18 * 0x70000 is exactly the file's size.
constexpr size_t kFeAlbumTrackSize = 0x70000;
constexpr size_t kFeAlbumTrackCount = 18;

// ---------------------------------------------------------------------------------------------
// DATA\AUDTAUNT.STR - rider taunt speech
// ---------------------------------------------------------------------------------------------

constexpr size_t kTauntRecordSize = 0x4000;
constexpr size_t kTauntHeaderSize = 0x70;

// A record body holds one or two complete SPU samples, each delimited the way the SPU itself does it:
// the first block carries the loop-start flag (0x04), the last audio block carries the loop-end flag
// (0x01), and one dummy terminator block `00 07 77 77 ...` follows it. Proven on all 150 records of
// the USA disc: 277 samples, 277 byte-identical terminator blocks, no exceptions.
struct TauntSample {
    uint32_t id = 0;               // +0x20 for the first sample, +0x24 for the second
    size_t offset = 0;             // file offset of the first ADPCM block
    size_t blocks = 0;             // audio blocks, the terminator NOT counted
    size_t size = 0;               // blocks * 16
    size_t terminatorOffset = 0;   // file offset of the `00 07 77...` block
    // Read from the record's own engine descriptor, not assumed: 0x0400 (11025 Hz) on 148 records,
    // 0x02E7 (7999 Hz) on records 141 and 142.
    uint16_t pitch = 0;
    int sampleRate = 0;            // SpuPitchToRate(pitch)
};

struct TauntRecord {
    size_t index = 0;
    size_t offset = 0;
    uint8_t soundId = 0;  // +0x00, one of {3,4,6,7,22,23,32,33}
    uint8_t voiceId = 0;  // +0x01, 0x81..0x9B
    uint32_t id20 = 0;    // +0x20, = soundId
    uint32_t id24 = 0;    // +0x24, usually = id20, 255 where there is only one sample
    uint32_t volume = 0;  // +0x34, u16, 0x3FC0 on every record - looks like an SPU volume
    uint8_t sampleCount = 0; // +0x2C (repeated at +0x2D and +0x2E, then 0x7F at +0x2F)
    // +0x38: where the per-sample descriptor list starts, relative to +0x28. 20 for a one-sample
    // record, 24 for a two-sample one - which is why the descriptors are NOT at a fixed offset.
    uint32_t descriptorListOffset = 0;
    // +0x5C: the body offset at which the SECOND sample starts, 0 when the record holds only one.
    // (It is NOT the length of the record's audio: the sample that begins there runs on past it.)
    uint32_t secondSampleOffset = 0;
    size_t dataOffset = 0;  // offset + 0x70
    std::vector<TauntSample> samples; // 1 or 2
};

std::vector<TauntRecord> TauntRecords(std::span<const uint8_t> file);
PcmBuffer DecodeTaunt(std::span<const uint8_t> file, const TauntSample& sample,
                      AdpcmState* stateOut = nullptr);

// The span tools\scout\audio.py decodes: exactly [+0x70, +0x70 + field(+0x5C)). That is the first
// sample plus any filler block before the second one, and nothing at all for the 23 records whose
// field is 0. Kept only so the two decoders can be diffed sample-for-sample - use DecodeTaunt().
PcmBuffer DecodeTauntProbeSpan(std::span<const uint8_t> file, const TauntRecord& record,
                               AdpcmState* stateOut = nullptr);

// ---------------------------------------------------------------------------------------------

enum class AudioFamily { Unknown, Wve, Alb, Taunt };

// `name` is only consulted for the families that have no magic (.ALB and AUDTAUNT.STR have none).
AudioFamily DetectAudioFamily(const std::string& name, std::span<const uint8_t> file);
const char* AudioFamilyName(AudioFamily family);

} // namespace rr
