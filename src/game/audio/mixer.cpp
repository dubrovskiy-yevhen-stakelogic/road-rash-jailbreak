#include "game/audio/mixer.h"

#include <algorithm>
#include <cstring>
#include <span>

#include "rrformats/audio.h"

namespace rr::audio {
namespace {

constexpr uint64_t kOne32 = 1ull << 32;

int32_t Clamp16(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return v;
}

} // namespace

// ------------------------------------------------------------------------------------------------

MemorySource::MemorySource(std::vector<int16_t> samples, int channels, int sourceRate)
    : samples_(std::move(samples)), channels_(channels), rate_(sourceRate) {
    if (channels_ < 1 || channels_ > 2) channels_ = 1;
    frames_ = samples_.size() / static_cast<size_t>(channels_);
}

size_t MemorySource::Read(int16_t* dst, size_t frames) {
    const size_t n = std::min(frames, frames_ - pos_);
    if (n) std::memcpy(dst, samples_.data() + pos_ * static_cast<size_t>(channels_),
                       n * static_cast<size_t>(channels_) * sizeof(int16_t));
    pos_ += n;
    return n;
}

// ------------------------------------------------------------------------------------------------

void PanGains(int volume, int pan, int32_t& leftGain, int32_t& rightGain) {
    volume = std::clamp(volume, 0, kVolumeUnit);
    pan = std::clamp(pan, -kVolumeUnit, kVolumeUnit);
    // Balance law: unity on both sides at the centre, one side fading to silence at the extremes.
    const int32_t l = pan <= 0 ? kVolumeUnit : kVolumeUnit - pan;
    const int32_t r = pan >= 0 ? kVolumeUnit : kVolumeUnit + pan;
    leftGain = static_cast<int32_t>((static_cast<int64_t>(volume) * l) >> 15);
    rightGain = static_cast<int32_t>((static_cast<int64_t>(volume) * r) >> 15);
}

Mixer::Mixer(int outputRate, size_t maxVoices) : outputRate_(outputRate > 0 ? outputRate : 44100) {
    voices_.resize(maxVoices == 0 ? 1 : maxVoices);
    for (Voice& v : voices_) v.buf.resize(kRefillFrames * 2);
}

void Mixer::RecomputeStep(Voice& v) const {
    const int64_t pitch = std::clamp(v.pitch, 1, 16 * kPitchUnit);
    const uint64_t num = static_cast<uint64_t>(v.source->SourceRate()) * static_cast<uint64_t>(pitch);
    const uint64_t den = static_cast<uint64_t>(outputRate_) * static_cast<uint64_t>(kPitchUnit);
    v.step = den ? (num << 32) / den : 0;
}

Mixer::Voice* Mixer::Find(VoiceId id) {
    if (!id) return nullptr;
    for (Voice& v : voices_)
        if (v.active && v.id == id) return &v;
    return nullptr;
}

const Mixer::Voice* Mixer::Find(VoiceId id) const {
    if (!id) return nullptr;
    for (const Voice& v : voices_)
        if (v.active && v.id == id) return &v;
    return nullptr;
}

VoiceId Mixer::Play(const VoiceDesc& desc) {
    if (!desc.source) return 0;
    const int channels = desc.source->Channels();
    if (channels < 1 || channels > 2) return 0;
    if (desc.source->SourceRate() <= 0) return 0;

    std::lock_guard<std::mutex> lock(mutex_);
    for (Voice& v : voices_) {
        if (v.active) continue;
        v.id = nextId_++;
        if (nextId_ == 0) nextId_ = 1;
        v.source = desc.source;
        v.channels = channels;
        v.volume = std::clamp(desc.volume, 0, kVolumeUnit);
        v.pan = std::clamp(desc.pan, -kVolumeUnit, kVolumeUnit);
        v.pitch = std::clamp(desc.pitch, 1, 16 * kPitchUnit);
        v.loop = desc.loop && desc.source->CanRewind();
        v.active = true;
        v.frac = 0;
        v.cur[0] = v.cur[1] = 0;
        v.nxt[0] = v.nxt[1] = 0;
        v.bufFrames = 0;
        v.bufPos = 0;
        v.buf.assign(kRefillFrames * static_cast<size_t>(channels), 0);
        RecomputeStep(v);
        // Prime the two-frame window. A source that cannot give a single frame never starts.
        if (!NextFrame(v, v.cur)) {
            v.active = false;
            v.source.reset();
            return 0;
        }
        // A source of exactly one frame: hold it, the voice ends on the next step.
        if (!NextFrame(v, v.nxt)) {
            v.nxt[0] = v.cur[0];
            v.nxt[1] = v.cur[1];
        }
        return v.id;
    }
    return 0;
}

bool Mixer::Stop(VoiceId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    Voice* v = Find(id);
    if (!v) return false;
    v->active = false;
    v->source.reset();
    return true;
}

void Mixer::StopAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (Voice& v : voices_) {
        v.active = false;
        v.source.reset();
    }
}

bool Mixer::IsPlaying(VoiceId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return Find(id) != nullptr;
}

bool Mixer::SetVolume(VoiceId id, int volume) {
    std::lock_guard<std::mutex> lock(mutex_);
    Voice* v = Find(id);
    if (!v) return false;
    v->volume = std::clamp(volume, 0, kVolumeUnit);
    return true;
}

bool Mixer::SetPan(VoiceId id, int pan) {
    std::lock_guard<std::mutex> lock(mutex_);
    Voice* v = Find(id);
    if (!v) return false;
    v->pan = std::clamp(pan, -kVolumeUnit, kVolumeUnit);
    return true;
}

bool Mixer::SetPitch(VoiceId id, int pitch) {
    std::lock_guard<std::mutex> lock(mutex_);
    Voice* v = Find(id);
    if (!v) return false;
    v->pitch = std::clamp(pitch, 1, 16 * kPitchUnit);
    RecomputeStep(*v);
    return true;
}

size_t Mixer::ActiveVoices() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t n = 0;
    for (const Voice& v : voices_)
        if (v.active) ++n;
    return n;
}

bool Mixer::NextFrame(Voice& v, int32_t out[2]) {
    if (v.bufPos >= v.bufFrames) {
        v.bufPos = 0;
        v.bufFrames = v.source->Read(v.buf.data(), kRefillFrames);
        if (v.bufFrames == 0) {
            if (!v.loop) return false;
            v.source->Rewind();
            v.bufFrames = v.source->Read(v.buf.data(), kRefillFrames);
            if (v.bufFrames == 0) return false; // an empty source must not spin
        }
    }
    const int16_t* frame = v.buf.data() + v.bufPos * static_cast<size_t>(v.channels);
    ++v.bufPos;
    if (v.channels == 1) {
        out[0] = out[1] = frame[0];
    } else {
        out[0] = frame[0];
        out[1] = frame[1];
    }
    return true;
}

void Mixer::Mix(int16_t* out, size_t frames) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (accum_.size() < frames * 2) accum_.assign(frames * 2, 0);
    else std::fill_n(accum_.begin(), frames * 2, 0);

    for (Voice& v : voices_) {
        if (!v.active) continue;
        int32_t gl = 0, gr = 0;
        PanGains(v.volume, v.pan, gl, gr);
        for (size_t i = 0; i < frames; ++i) {
            const int64_t f = static_cast<int64_t>(v.frac >> 16); // Q16 of the inter-frame position
            const int32_t l =
                static_cast<int32_t>(v.cur[0] + (((static_cast<int64_t>(v.nxt[0]) - v.cur[0]) * f) >> 16));
            const int32_t r =
                static_cast<int32_t>(v.cur[1] + (((static_cast<int64_t>(v.nxt[1]) - v.cur[1]) * f) >> 16));
            accum_[i * 2] += static_cast<int32_t>((static_cast<int64_t>(l) * gl) >> 15);
            accum_[i * 2 + 1] += static_cast<int32_t>((static_cast<int64_t>(r) * gr) >> 15);

            v.frac += v.step;
            while (v.frac >= kOne32) {
                v.frac -= kOne32;
                v.cur[0] = v.nxt[0];
                v.cur[1] = v.nxt[1];
                if (!NextFrame(v, v.nxt)) {
                    v.active = false;
                    v.source.reset();
                    break;
                }
            }
            if (!v.active) break;
        }
    }

    for (size_t i = 0; i < frames * 2; ++i) out[i] = static_cast<int16_t>(Clamp16(accum_[i]));
}

// ============================================================================ SpuVoices
SpuVoices::SpuVoices() : ram_(kRamBytes, 0) {}

uint16_t SpuVoices::Read(uint32_t offset) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return regs_[(offset >> 1) & 0xFFu];
}

void SpuVoices::Write(uint32_t offset, uint16_t value) {
    std::lock_guard<std::mutex> lock(mutex_);
    offset &= 0x1FEu;
    regs_[offset >> 1] = value;
    if (offset < 0x180u) {
        Voice& v = v_[offset >> 4];
        switch (offset & 0xFu) {
        case 0x0: v.volL = value; break;
        case 0x2: v.volR = value; break;
        case 0x4: v.pitch = value; break;
        case 0x6: v.start = value; break;
        case 0x8: v.adsr1 = value; break; // B4
        case 0xA: v.adsr2 = value; break; // B4
        case 0xE: v.repeatReg = value; v.repeat = static_cast<uint32_t>(value) * 8u; break; // B6
        default: break; // the current ADSR volume (+C) is the SPU's to write, not the CPU's
        }
        return;
    }
    const uint32_t lohi = (offset & 2u) ? 16u : 0u;
    if (offset == 0x188u || offset == 0x18Au) { // KON
        for (uint32_t b = 0; b < 16; ++b)
            if ((value >> b) & 1u) KeyOn(static_cast<int>(b + lohi));
    } else if (offset == 0x18Cu || offset == 0x18Eu) { // KOFF: the release phase (B4)
        for (uint32_t b = 0; b < 16; ++b) {
            const uint32_t ch = b + lohi;
            if (((value >> b) & 1u) && ch < static_cast<uint32_t>(kChannels) && v_[ch].on)
                Enter(v_[ch], Phase::Release);
        }
    }
    // PMON (0x190/0x192) is read from regs_ on every sample - see Render.
}

void SpuVoices::KeyOn(int ch) {
    if (ch < 0 || ch >= kChannels) return;
    Voice& v = v_[ch];
    v.on = true;
    v.addr = static_cast<uint32_t>(v.start) * 8u;
    v.pos = 0;
    v.counter = 0;
    v.prev1 = v.prev2 = 0;
    v.advanced = 0;
    v.level = 0;
    Enter(v, Phase::Attack); // B4: every key-on restarts the envelope from 0
    ++keyOns_;
    if (traceKeyOns_ && keyOnTrace_.size() < 1024)
        keyOnTrace_.push_back(KeyOnEvent{ch, v.addr, v.pitch, v.volL, v.volR, v.adsr1, v.adsr2});
    EnterBlock(v);
}

// B4, one sample of the envelope. The rates are the SPU's documented rule: a step waits
// 1 << max(0, shift - 11) samples and adds step << max(0, 11 - shift); exponential attack is four
// times slower above 0x6000, exponential decrease scales the step by level / 0x8000.
// The documented loop, per phase: { cycles, step from the level NOW; wait(cycles); level += step }.
// `Rate` is its first line for the voice's current phase and level; `Envelope` waits out `wait`
// and then adds, and computes the next wait from the level the add left.
void SpuVoices::Rate(const Voice& v, int32_t& cycles, int32_t& add) {
    bool expo = false, down = false;
    int32_t shift = 0, step = 0;
    switch (v.phase) {
    case Phase::Attack:
        expo = (v.adsr1 >> 15) & 1u;
        shift = (v.adsr1 >> 10) & 0x1F;
        step = 7 - static_cast<int32_t>((v.adsr1 >> 8) & 3u);
        break;
    case Phase::Decay:
        expo = true;
        down = true;
        shift = (v.adsr1 >> 4) & 0xF;
        step = -8;
        break;
    case Phase::Sustain: {
        expo = (v.adsr2 >> 15) & 1u;
        down = (v.adsr2 >> 14) & 1u;
        shift = (v.adsr2 >> 8) & 0x1F;
        const int32_t s = static_cast<int32_t>((v.adsr2 >> 6) & 3u);
        step = down ? -8 + s : 7 - s;
        break;
    }
    case Phase::Release:
        expo = (v.adsr2 >> 5) & 1u;
        down = true;
        shift = v.adsr2 & 0x1F;
        step = -8;
        break;
    case Phase::Off:
        cycles = 1;
        add = 0;
        return;
    }
    cycles = 1 << std::max(0, shift - 11);
    add = step * (1 << std::max(0, 11 - shift));
    if (expo && !down && v.level > 0x6000) cycles *= 4;
    if (expo && down) add = (add * v.level) >> 15;
}

void SpuVoices::Enter(Voice& v, Phase phase) {
    v.phase = phase;
    int32_t cycles = 1, add = 0;
    Rate(v, cycles, add);
    v.wait = cycles - 1;
}

void SpuVoices::Envelope(Voice& v) {
    if (v.phase == Phase::Off) return;
    if (v.wait > 0) {
        --v.wait;
        return;
    }
    int32_t cycles = 1, add = 0;
    Rate(v, cycles, add);
    v.level = std::clamp(v.level + add, 0, 0x7FFF);
    const int32_t sustain = (static_cast<int32_t>(v.adsr1 & 0xFu) + 1) * 0x800;
    Phase next = v.phase;
    switch (v.phase) {
    case Phase::Attack:
        // at the top the decay begins - "until the level is at or below the sustain level", so a
        // sustain level of 0xF (0x8000) has no decay step at all
        if (v.level >= 0x7FFF) next = (v.level <= sustain) ? Phase::Sustain : Phase::Decay;
        break;
    case Phase::Decay:
        if (v.level <= sustain) next = Phase::Sustain;
        break;
    case Phase::Release:
        if (v.level <= 0) {
            v.phase = Phase::Off;
            v.on = false;
            v.out = 0;
            return;
        }
        break;
    default: break;
    }
    Enter(v, next);
}

int SpuVoices::SoundingVoices() const {
    std::lock_guard<std::mutex> lock(mutex_);
    int n = 0;
    for (const Voice& v : v_)
        if (v.on && v.level > 0 && (v.volL & 0x7FFFu) != 0) ++n;
    return n;
}

void SpuVoices::EnterBlock(Voice& v) {
    v.addr &= kRamBytes - 1u;
    v.addr &= ~15u;
    const uint8_t* b = ram_.data() + v.addr;
    v.flags = b[1];
    if (v.flags & 4u) v.repeat = v.addr; // loop start: the block copies its own address (B6)
    rr::AdpcmState st;
    st.prev1 = v.prev1;
    st.prev2 = v.prev2;
    st.lenient = true; // the hardware decodes whatever is there
    std::vector<int16_t> out;
    rr::DecodeAdpcm(std::span<const uint8_t>(b, rr::kSpuBlockSize), rr::AdpcmBlock::Spu16, st, out);
    for (int i = 0; i < 28; ++i) v.samples[i] = (i < static_cast<int>(out.size())) ? out[static_cast<size_t>(i)] : 0;
    v.prev1 = st.prev1;
    v.prev2 = st.prev2;
    v.pos = 0;
}

void SpuVoices::NextBlock(Voice& v) {
    const uint32_t old = v.addr;
    if (v.flags & 1u) {            // loop end
        v.addr = v.repeat;
        if (!(v.flags & 2u)) {     // ... without repeat: the voice ends
            v.on = false;
            v.out = 0;
            return;
        }
    } else {
        v.addr += 16u;
    }
    // a streamed ring: the chunk the voice just left is refilled
    for (int r = 0; r < 2; ++r) {
        Ring& ring = rings_[r];
        if (!ring.fill || ring.chunkBytes == 0) continue;
        const uint32_t end = ring.base + ring.chunkBytes * static_cast<uint32_t>(ring.chunks);
        if (old < ring.base || old >= end) continue;
        const uint32_t oc = (old - ring.base) / ring.chunkBytes;
        const bool inside = v.addr >= ring.base && v.addr < end;
        const uint32_t nc = inside ? (v.addr - ring.base) / ring.chunkBytes : 0xFFFFFFFFu;
        if (nc != oc && ring.base + ring.chunkBytes * (oc + 1u) <= kRamBytes) {
            ring.fill(r, static_cast<int>(oc), ram_.data() + ring.base + ring.chunkBytes * oc, ring.chunkBytes);
            ++refills_;
        }
    }
    EnterBlock(v);
}

void SpuVoices::SetStreamRing(int ring, uint32_t base, uint32_t chunkBytes, int chunks, StreamFill fill) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ring < 0 || ring > 1) return;
    rings_[ring].base = base;
    rings_[ring].chunkBytes = chunkBytes;
    rings_[ring].chunks = chunks;
    rings_[ring].fill = std::move(fill);
    // the whole ring, primed
    for (int c = 0; c < chunks; ++c) {
        const uint32_t at = base + chunkBytes * static_cast<uint32_t>(c);
        if (at + chunkBytes > kRamBytes || !rings_[ring].fill) break;
        rings_[ring].fill(ring, c, ram_.data() + at, chunkBytes);
    }
}

namespace {
// A voice volume register in fixed mode: bits 0..14 are volume/2 as a signed 15-bit number.
int32_t SpuVolume(uint16_t reg) {
    if (reg & 0x8000u) return 0; // sweep mode is not modelled; the game only writes fixed volumes
    const int32_t v15 = static_cast<int32_t>(reg & 0x7FFFu);
    return ((v15 ^ 0x4000) - 0x4000) * 2;
}
} // namespace

void SpuVoices::Render(int16_t* out, size_t frames) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t f = 0; f < frames; ++f) {
        const uint32_t pmon = static_cast<uint32_t>(regs_[0x190 >> 1]) |
                              (static_cast<uint32_t>(regs_[0x192 >> 1]) << 16);
        int32_t mixL = 0, mixR = 0;
        for (int ch = 0; ch < kChannels; ++ch) { // in channel order: n-1's output is this sample's
            Voice& v = v_[ch];
            if (!v.on) {
                v.out = 0;
                continue;
            }
            Envelope(v); // B4
            if (!v.on) continue;
            v.out = static_cast<int16_t>((static_cast<int32_t>(v.samples[v.pos]) * v.level) >> 15);
            mixL += (static_cast<int32_t>(v.out) * SpuVolume(v.volL)) >> 15;
            mixR += (static_cast<int32_t>(v.out) * SpuVolume(v.volR)) >> 15;
            uint32_t step = v.pitch;
            if (ch > 0 && ((pmon >> ch) & 1u)) { // B5
                const int32_t factor = static_cast<int32_t>(v_[ch - 1].out) + 0x8000;
                const int32_t sext = static_cast<int32_t>(static_cast<int16_t>(step));
                step = static_cast<uint32_t>((sext * factor) >> 15) & 0xFFFFu;
            }
            if (step > 0x3FFFu) step = 0x4000u;
            v.counter += step;
            while (v.counter >= 0x1000u) {
                v.counter -= 0x1000u;
                ++v.advanced;
                if (++v.pos >= 28) {
                    NextBlock(v);
                    if (!v.on) break;
                }
            }
        }
        // the main volume, after the clamp of the voice sum (0x1F801D80/82, fixed mode)
        const int32_t l = Clamp16((Clamp16(mixL) * SpuVolume(regs_[0x180 >> 1])) >> 15);
        const int32_t r = Clamp16((Clamp16(mixR) * SpuVolume(regs_[0x182 >> 1])) >> 15);
        peak_ = std::max(peak_, std::max(l < 0 ? -l : l, r < 0 ? -r : r));
        // saturation is where the SUM leaves 16 bits; after the main volume the rail itself is
        // not reachable at 0x3FFF (32767 * 0x7FFE >> 15 = 32766)
        clipped_ += static_cast<uint64_t>(mixL > 32767 || mixL < -32768) +
                    static_cast<uint64_t>(mixR > 32767 || mixR < -32768);
        rendered_ += 2;
        out[2 * f] = static_cast<int16_t>(l);
        out[2 * f + 1] = static_cast<int16_t>(r);
    }
}

void SpuVoices::TraceKeyOns(bool on) {
    std::lock_guard<std::mutex> lock(mutex_);
    traceKeyOns_ = on;
    if (!on) keyOnTrace_.clear();
}

std::vector<SpuVoices::KeyOnEvent> SpuVoices::TakeKeyOns() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<KeyOnEvent> out;
    out.swap(keyOnTrace_);
    return out;
}

SpuVoices::VoiceState SpuVoices::State(int channel) const {
    std::lock_guard<std::mutex> lock(mutex_);
    VoiceState s;
    if (channel < 0 || channel >= kChannels) return s;
    const Voice& v = v_[channel];
    s.on = v.on;
    s.address = v.addr;
    s.repeat = v.repeat;
    s.out = v.out;
    s.advanced = v.advanced;
    s.phase = v.phase;
    s.level = v.level;
    return s;
}

} // namespace rr::audio
