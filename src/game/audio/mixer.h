#pragma once
// Deterministic software mixer for the native game.
//
// Everything is integer: the output of Mix() is a pure function of the voice list, the parameters and
// the sources' sample streams, so a mixed buffer can be compared bit-for-bit between runs and between
// machines. No floating point, no dependency beyond the standard library.
//
// Fixed-point conventions (chosen to match the hardware this game was written for):
//   pitch   Q12, 0x1000 = play the source at its own sample rate (the PS1 SPU's own pitch register)
//   volume  Q15, 0x8000 = unity
//   pan     Q15, -0x8000 = hard left, 0 = centre, +0x8000 = hard right (balance law: unity on both
//           sides at the centre, the far side fading linearly to silence)
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace rr::audio {

// A source of interleaved 16-bit PCM. Read() is called from the mixer thread.
class PcmSource {
public:
    virtual ~PcmSource() = default;
    virtual int Channels() const = 0;
    virtual int SourceRate() const = 0;
    // Writes up to `frames` frames (Channels() samples each) and returns how many it produced.
    // A short read (including 0) means the source is exhausted.
    virtual size_t Read(int16_t* dst, size_t frames) = 0;
    virtual bool CanRewind() const { return false; }
    virtual void Rewind() {}
};

// A source over a decoded buffer that the caller keeps alive through the shared_ptr.
class MemorySource final : public PcmSource {
public:
    MemorySource(std::vector<int16_t> samples, int channels, int sourceRate);
    int Channels() const override { return channels_; }
    int SourceRate() const override { return rate_; }
    size_t Read(int16_t* dst, size_t frames) override;
    bool CanRewind() const override { return true; }
    void Rewind() override { pos_ = 0; }
    size_t Frames() const { return frames_; }

private:
    std::vector<int16_t> samples_;
    int channels_;
    int rate_;
    size_t frames_ = 0;
    size_t pos_ = 0; // in frames
};

constexpr int kPitchUnit = 0x1000;
constexpr int kVolumeUnit = 0x8000;

struct VoiceDesc {
    std::shared_ptr<PcmSource> source;
    int pitch = kPitchUnit;    // Q12
    int volume = kVolumeUnit;  // Q15
    int pan = 0;               // Q15, signed
    bool loop = false;         // requires source->CanRewind()
};

using VoiceId = uint32_t; // 0 is never a valid id

class Mixer {
public:
    explicit Mixer(int outputRate, size_t maxVoices = 24);

    int OutputRate() const { return outputRate_; }
    size_t MaxVoices() const { return voices_.size(); }

    // Returns 0 when every slot is busy or the description is unusable.
    VoiceId Play(const VoiceDesc& desc);
    bool Stop(VoiceId id);
    void StopAll();
    bool IsPlaying(VoiceId id) const;
    bool SetVolume(VoiceId id, int volume);
    bool SetPan(VoiceId id, int pan);
    bool SetPitch(VoiceId id, int pitch);
    size_t ActiveVoices() const;

    // Mixes `frames` stereo frames into `out` (interleaved L,R), overwriting it.
    void Mix(int16_t* out, size_t frames);

private:
    static constexpr size_t kRefillFrames = 256;

    struct Voice {
        VoiceId id = 0;
        std::shared_ptr<PcmSource> source;
        int channels = 0;
        int volume = kVolumeUnit;
        int pan = 0;
        int pitch = kPitchUnit;
        bool loop = false;
        bool active = false;
        uint64_t step = 0;      // Q32 source frames per output frame
        uint64_t frac = 0;      // Q32 position between cur and next
        int32_t cur[2] = {0, 0};
        int32_t nxt[2] = {0, 0};
        std::vector<int16_t> buf; // kRefillFrames * channels
        size_t bufFrames = 0;
        size_t bufPos = 0;
    };

    void RecomputeStep(Voice& v) const;
    bool NextFrame(Voice& v, int32_t out[2]); // false when the source is finished
    Voice* Find(VoiceId id);
    const Voice* Find(VoiceId id) const;

    int outputRate_;
    VoiceId nextId_ = 1;
    std::vector<Voice> voices_;
    std::vector<int32_t> accum_;
    mutable std::mutex mutex_;
};

// Splits a volume/pan pair into the two Q15 channel gains. Exposed because the game will want to feed
// the same law into whatever writes the per-voice registers.
void PanGains(int volume, int pan, int32_t& leftGain, int32_t& rightGain);

// ============================================================================ the SPU voice model
//
// Items B5 and B6: the two SPU voice features the engine note uses and a "play this decoded buffer"
// mixer cannot express.
//
//   B5  PITCH MODULATION. With bit n of PMON (0x1F801D90/92) set, channel n's pitch step is scaled
//       by channel n-1's CURRENT OUTPUT SAMPLE (after the envelope, before the volume):
//           step = (sign-extend16(pitch) * (out[n-1] + 0x8000)) >> 15, masked to 16 bits,
//       then clamped to 0x4000 like every step. Channel 0 has no channel below it and is never
//       modulated. This is why a voice is addressed by its CHANNEL here and not by a handle: the
//       engine's layer 4 modulates layer 3 only because the allocator put it one channel below.
//   B6  THE REPEAT ADDRESS is a live register. At the end of a block whose flags have bit 0 (loop
//       end) the voice continues at the repeat address (and stops if bit 1, repeat, is clear); a
//       block whose flags have bit 2 (loop start) copies its own address INTO the repeat address as
//       the voice enters it. So a write while the voice plays takes effect at the next loop end, and
//       the new loop's own loop-start block then keeps the voice there. The engine samples are built
//       for exactly this: sub-loops that each open with flags 6 and close with flags 3.
//
// The voice is driven by the SPU's own REGISTERS (`Write`), in the SPU's own units, because that is
// what the ported libspu of src\game\sim\sound_engine.h produces: the voice block
// 0x1F801C00 + 16n (+0 volume L, +2 volume R, +4 pitch, +6 start address / 8, +8/+A ADSR, +E repeat
// address / 8), KON 0x1F801D88/8A, KOFF 0x1F801D8C/8E, PMON 0x1F801D90/92. Samples are SPU-ADPCM in
// `Ram()`, decoded block by block as the voice reaches them with the project's one proven decoder
// (rrformats/audio.h), carrying each voice's own filter history across a loop jump as the hardware
// does - which a pre-decoded buffer cannot.
//
//   B4  THE ADSR ENVELOPE, from the two ADSR registers (+8, +A) as the SPU documents
//       them: key-on starts the attack at level 0; attack (linear or exponential, rate from shift
//       and step, x4 slower above 0x6000 when exponential) to 0x7FFF, then an exponential decay to
//       the sustain level ((ADSR1 & 0xF) + 1) * 0x800, then the sustain (linear or exponential,
//       up or down) until key-off, which starts the release (linear or exponential) to 0 - and at 0
//       the voice ends. Each step waits `1 << max(0, shift - 11)` samples and adds
//       `step << max(0, 11 - shift)`, scaled by level / 0x8000 when exponential and decreasing.
//       The voice's output is `sample * level >> 15`: that is what the volume multiplies and what
//       PMON reads from the channel below.
//   MAIN VOLUME (0x1F801D80/82): the sum of the voices is clamped to 16 bits and then scaled by
//       the main volume (fixed mode, the same volume/2 encoding as a voice's), then clamped again.
//
// What it does NOT model, named: the SPU's 4-point Gaussian interpolation (the sample under the
// counter is used), reverb, noise, volume sweeps and IRQs. Output is 44 100 Hz stereo, integer only,
// a pure function of the register writes and of `Render` calls.
class SpuVoices {
public:
    static constexpr int kChannels = 24;
    static constexpr int kRate = 44100;
    static constexpr uint32_t kRamBytes = 512u * 1024u;

    SpuVoices();
    // The SPU's sample RAM, byte-addressed exactly as the start and repeat registers address it.
    std::vector<uint8_t>& Ram() { return ram_; }

    // A 16-bit write to the register at `offset` = address - 0x1F801C00 (0x000..0x1FF). Registers
    // this model has no use for are stored and otherwise ignored.
    void Write(uint32_t offset, uint16_t value);
    uint16_t Read(uint32_t offset) const;

    // Renders `frames` stereo frames at kRate, interleaved L,R, overwriting `out`.
    void Render(int16_t* out, size_t frames);

    // A streamed ring: `chunks` chunks of `chunkBytes` at `base` in SPU RAM. When a
    // voice playing inside the ring leaves a chunk, `fill(ring, chunk, dst, bytes)` refills that
    // chunk - the CD / DMA / SPU-IRQ side of the original's stream player, which is hardware and
    // library work, not the game's. Called on the rendering thread, under this model's lock.
    using StreamFill = std::function<void(int ring, int chunk, uint8_t* dst, uint32_t bytes)>;
    void SetStreamRing(int ring, uint32_t base, uint32_t chunkBytes, int chunks, StreamFill fill);
    uint64_t StreamRefills() const { return refills_; }

    // Observation, for tests and probes.
    enum class Phase : uint8_t { Off, Attack, Decay, Sustain, Release };
    struct VoiceState {
        bool on = false;
        uint32_t address = 0;  // the block being played, in bytes
        uint32_t repeat = 0;   // the repeat address, in bytes
        int16_t out = 0;       // the last output sample, after the envelope, before volume
        uint64_t advanced = 0; // samples consumed since key-on
        Phase phase = Phase::Off;
        int32_t level = 0;     // the envelope, 0..0x7FFF
    };
    VoiceState State(int channel) const;
    uint32_t KeyOns() const { return keyOns_; }
    // Observation: every key-on with the registers it started from, kept
    // only while tracing is on (at most 1024 between two takes; the rest are counted, not kept).
    struct KeyOnEvent {
        int channel = 0;
        uint32_t address = 0; // the start register, in bytes
        uint16_t pitch = 0, volL = 0, volR = 0, adsr1 = 0, adsr2 = 0;
    };
    void TraceKeyOns(bool on);
    std::vector<KeyOnEvent> TakeKeyOns();
    int32_t Peak() const { return peak_; }
    // Output samples whose voice SUM left 16 bits (clamped before the main volume), and all output
    // samples rendered - a measurement of saturation, not a correction of it.
    uint64_t Clipped() const { return clipped_; }
    uint64_t Rendered() const { return rendered_; }
    int SoundingVoices() const;

private:
    struct Voice {
        uint16_t volL = 0, volR = 0, pitch = 0, start = 0, repeatReg = 0, adsr1 = 0, adsr2 = 0;
        bool on = false;
        uint32_t addr = 0, repeat = 0;
        uint8_t flags = 0;
        int pos = 0;
        uint32_t counter = 0;
        int16_t samples[28] = {};
        int32_t prev1 = 0, prev2 = 0;
        int16_t out = 0;
        uint64_t advanced = 0;
        Phase phase = Phase::Off;
        int32_t level = 0;
        int32_t wait = 0; // samples left before the next envelope step
    };
    void KeyOn(int ch);
    void EnterBlock(Voice& v);
    void NextBlock(Voice& v);
    static void Envelope(Voice& v);
    static void Rate(const Voice& v, int32_t& cycles, int32_t& add);
    static void Enter(Voice& v, Phase phase);

    std::vector<uint8_t> ram_;
    uint16_t regs_[256] = {};
    Voice v_[kChannels];
    uint32_t keyOns_ = 0;
    bool traceKeyOns_ = false;
    std::vector<KeyOnEvent> keyOnTrace_;
    int32_t peak_ = 0;
    uint64_t clipped_ = 0, rendered_ = 0;
    struct Ring {
        uint32_t base = 0, chunkBytes = 0;
        int chunks = 0;
        StreamFill fill;
    };
    Ring rings_[2];
    uint64_t refills_ = 0;
    mutable std::mutex mutex_;
};

// The SPU voice model as a source for `Mixer`: stereo, kRate, never exhausted.
class SpuVoicesSource final : public PcmSource {
public:
    explicit SpuVoicesSource(std::shared_ptr<SpuVoices> spu) : spu_(std::move(spu)) {}
    int Channels() const override { return 2; }
    int SourceRate() const override { return SpuVoices::kRate; }
    size_t Read(int16_t* dst, size_t frames) override {
        spu_->Render(dst, frames);
        return frames;
    }

private:
    std::shared_ptr<SpuVoices> spu_;
};

} // namespace rr::audio
