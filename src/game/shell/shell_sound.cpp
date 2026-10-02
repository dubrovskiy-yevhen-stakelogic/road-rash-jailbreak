// The front end's sound (shell_sound.h).
#include "game/shell/shell_sound.h"

#include "game/sim/sound_engine.h"
#include "game/sim/sound_frame.h"

#include <algorithm>
#include <cstring>

namespace rr::shell {

namespace {
constexpr uint32_t kRamBytes = 2u * 1024u * 1024u;
constexpr uint32_t kSys = rr::sim::kSoundSystem;          // 0x800D6870
constexpr uint32_t kBankTable = 0x800FAD0C;               // the capture's S+0x04
constexpr uint32_t kVoices = 0x800FAD44;                  // the capture's S+0x0C, 24 x 44
constexpr uint32_t kBankRecord = 0x8016D8E4;              // the capture's *(0x800A0860), 0x13C bytes
constexpr uint32_t kSpuBankBase = 0x1010;                 // the capture's bank +0x08
constexpr uint32_t kUiBank = 0x800A0854;                  // s32 the shell bank's slot
constexpr uint32_t kTrack = 0x800A0830, kPending = 0x800A0838, kPlaying = 0x800A0834;
constexpr uint32_t kHandles = 0x800A0844;                 // the two music voices
constexpr uint32_t kFaded = 0x800A0840, kOffset = 0x800A0850, kChunks = 0x800A0858, kIssued = 0x800A0864;
constexpr uint32_t kLoading = 0x800A0878;
constexpr uint32_t kSpuLeft = 0xFFB0, kSpuRight = 0x47FB0; // 0x8007F028
constexpr uint32_t kTrackBytes = 0x70000, kChunk = 0x4000;
} // namespace

void JukeboxSet(GuestRam& g, uint32_t track, int32_t on) {
    if (static_cast<int32_t>(track) < 0 || track >= g.U32(0x80053578u)) return;
    const uint32_t a = 0x80053588u + 12u * track;
    g.W32(a, on != 0 ? (g.U32(a) | 1u) : (g.U32(a) & ~1u));
}

void SoundModeSet(GuestRam& g, uint32_t mono) { g.W32(0x800D69F4u, mono); }

// libspu's register traffic into the SPU voice model; the control block kept as a register file so
// libspu's read-modify-write of EON / PMON reads back what was written (SoundRuntime's rule); a root
// counter read answered from an LCG of our own.
class ShellSound::Io final : public rr::sim::SoundIo {
public:
    explicit Io(ShellSound& s) : s_(s) {}
    bool Load16(uint32_t address, uint16_t& out) override {
        const uint32_t reg = address & 0x1FFFFFFFu;
        if (reg >= 0x1F801D80u && reg < 0x1F801D9Cu) {
            out = s_.spuControl_[(reg - 0x1F801D80u) >> 1];
            return true;
        }
        if (reg == 0x1F801100u || reg == 0x1F801110u || reg == 0x1F801120u) {
            s_.timer_ = s_.timer_ * 0x41C64E6Du + 0x3039u;
            out = static_cast<uint16_t>(s_.timer_ >> 16);
            return true;
        }
        return false;
    }
    bool Store16(uint32_t address, uint16_t value) override {
        const uint32_t reg = address & 0x1FFFFFFFu;
        if (reg < 0x1F801C00u || reg >= 0x1F801E00u) return false;
        if (reg >= 0x1F801D80u && reg < 0x1F801D9Cu) s_.spuControl_[(reg - 0x1F801D80u) >> 1] = value;
        s_.spu_->Write(reg - 0x1F801C00u, value);
        return true;
    }

private:
    ShellSound& s_;
};

ShellSound::ShellSound(const DiscImage& disc) : disc_(disc), ram_(kRamBytes, 0), io_(std::make_unique<Io>(*this)) {
    const auto vuk = disc.Find("DATA/FRONTEND.VUK");
    const auto alb = disc.Find("DATA/FEALBUM.ALB");
    const auto slus = disc.Find("SLUS_010.53");
    if (!vuk || !alb || !slus) {
        error_ = "the disc has no DATA\\FRONTEND.VUK / DATA\\FEALBUM.ALB / SLUS_010.53";
        return;
    }
    bank_ = disc.ReadFile(*vuk);
    album_ = disc.ReadFile(*alb);
    exe_ = disc.ReadFile(*slus);
    // The bank: magic 2, the sound count at +4, the bank record 0x10 + 4 * count +
    // 16 * (non-empty sounds) bytes (0x13C, the size 0x8007E824 allocates for it), then the samples;
    // the file closes on it exactly.
    const uint32_t count = bank_.size() > 4 ? bank_[4] : 0u;
    size_t records = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t o = 0;
        std::memcpy(&o, bank_.data() + 0x10u + 4u * i, 4);
        records += o != 0 ? 1u : 0u;
    }
    uint32_t magic = 0, samples = 0;
    std::memcpy(&magic, bank_.data(), 4);
    std::memcpy(&samples, bank_.data() + 0x0C, 4);
    const size_t bankBytes = 0x10u + 4u * count + 16u * records;
    if (magic != 2u || count == 0 || bankBytes + samples != bank_.size() || bankBytes != 0x13Cu) {
        error_ = "DATA\\FRONTEND.VUK is not a 0x13C-byte bank record that closes on its own size";
        return;
    }
    if (exe_.size() <= 0x800 || album_.size() < kTrackBytes * 18u) {
        error_ = "SLUS_010.53 or DATA\\FEALBUM.ALB is too short";
        return;
    }
    ready_ = true;
    Reset();
}

ShellSound::~ShellSound() = default;

// The sound state at the original's addresses (the file header says which parts are OURS).
void ShellSound::Reset() {
    std::fill(ram_.begin(), ram_.end(), uint8_t{0});
    std::memcpy(ram_.data() + 0x10000, exe_.data() + 0x800, std::min<size_t>(exe_.size() - 0x800, kRamBytes - 0x10000));
    spu_ = std::make_shared<rr::audio::SpuVoices>();
    std::memset(spuControl_, 0, sizeof(spuControl_));
    GuestRam g(ram_.data(), rr::sim::kSoundGp);
    rr::sim::SoundMachine s{g, ram_.data(), *io_};
    rr::sim::ResetSoundState(s);                        // PORTED
    g.W32(kSys + 0x00, 12);                             // SoundInit's layout, the capture's blocks
    g.W32(kSys + 0x04, kBankTable);
    g.W32(kSys + 0x08, 24);
    g.W32(kSys + 0x0C, kVoices);
    for (uint32_t v = 0; v < 24u; ++v) g.W32(kVoices + 44u * v + 0x1Cu, v);
    // LoadBank SLUS 0x8001ED28 into the first free slot: the record, its SPU base, PatchBank (PORTED).
    g.WriteBlock(kBankRecord, bank_.data(), 0x13C);
    std::memcpy(spu_->Ram().data() + kSpuBankBase, bank_.data() + 0x13C, bank_.size() - 0x13C);
    g.W32(kBankRecord + 8u, kSpuBankBase);
    rr::sim::PatchBank(s, kBankRecord, kSpuBankBase);
    g.W32(kBankTable, kBankRecord);
    g.W32(kUiBank, 0);
    g.W32(kTrack, 0xFFFFFFFFu);                         // 0x8007E824
    g.W32(g.gp() + rr::sim::kDefaultBankGp, 0xFFFFFFFFu); // the capture's gp+1912
    spu_->Write(0x180, 0x3FFF);                         // the main volume every capture holds
    spu_->Write(0x182, 0x3FFF);
    track_ = -1;
    loading_ = false;
    if (s.Faulted()) error_ = "the sound state's reset faulted";
}

bool ShellSound::MusicPlaying() const {
    uint32_t v = 0;
    std::memcpy(&v, ram_.data() + (kPlaying & (kRamBytes - 1u)), 4);
    return v != 0;
}

// RASHCDF 0x8007EAC0.
void ShellSound::PlayUiSound(uint32_t n) {
    if (n >= 15u) return;
    GuestRam g(ram_.data(), rr::sim::kSoundGp);
    rr::sim::SoundMachine s{g, ram_.data(), *io_};
    rr::sim::SoundParams p;
    p.pitch = -1;
    p.volume = g.S32(0x800D6C0Cu);
    p.pan = 0x40;
    rr::sim::GuestStartVoice(s, g.S32(kUiBank), static_cast<int32_t>(n), 0, 0, p); // SLUS 0x8001F174
    ++clicks_;
}

// RASHCDF 0x8007EB1C: the sound options' preview of slider `kind` at `value` - kind 0 retunes the two
// music voices (UpdateVoice 0x8001F6A4, pitch 0x5CE, pans 0 / 0x7F), kinds 1..5 play (or retune) the
// bank's sound 5..9 at the scaled value (the tables 0x80052638..0x80052650 of the player's SLUS), kept in
// 0x800A085C while kind is 2..4 (a held, looping sample) and forgotten otherwise.
void ShellSound::SliderPreview(int32_t kind, int32_t value) {
    GuestRam g(ram_.data(), rr::sim::kSoundGp);
    rr::sim::SoundMachine s{g, ram_.data(), *io_};
    constexpr uint32_t kPreview = 0x800A085C;
    rr::sim::SoundParams p;
    if (kind == 0) {
        p.pitch = 0x5CE;
        p.volume = ((value * g.S32(0x8005264Cu)) >> 4) * g.S32(0x80052650u) >> 7;
        p.pan = 0;
        rr::sim::UpdateVoice(s, g.U32(kHandles), p);
        p.pan = 0x7F;
        rr::sim::UpdateVoice(s, g.U32(kHandles + 4u), p);
        return;
    }
    int32_t sound = 0, held = 0;
    static const uint32_t kScale[6] = {0, 0x80052644u, 0x80052638u, 0x8005263Cu, 0x80052640u, 0x80052648u};
    if (kind >= 1 && kind <= 5) {
        value = (value * g.S32(kScale[kind])) >> 4;
        sound = 4 + kind;
        held = (kind >= 2 && kind <= 4) ? 1 : 0;
    }
    p.pan = 0x40;
    p.volume = value * g.S32(0x80052650u) >> 7;
    p.pitch = static_cast<int32_t>(rr::sim::GetSoundPitch(s, g.S32(kUiBank), sound, 0));
    if (g.U32(kPreview) == 0)
        g.W32(kPreview, rr::sim::GuestStartVoice(s, g.S32(kUiBank), sound, 0, held, p));
    else
        rr::sim::UpdateVoice(s, g.U32(kPreview), p);
    if (held == 0) g.W32(kPreview, 0);
}

// RASHCDF 0x8007ECFC: the held preview stopped (StopVoice 0x8001F7EC).
void ShellSound::PreviewStop() {
    GuestRam g(ram_.data(), rr::sim::kSoundGp);
    rr::sim::SoundMachine s{g, ram_.data(), *io_};
    if (g.U32(0x800A085Cu) == 0) return;
    rr::sim::StopVoice(s, g.U32(0x800A085Cu));
    g.W32(0x800A085Cu, 0);
}

// RASHCDF 0x8007ED34: the stereo / mono option's sample - sound 10 centred for 0, else sound 11 hard
// left and 12 hard right, at the effects slider.
void ShellSound::SoundModeUi(int32_t mode) {
    GuestRam g(ram_.data(), rr::sim::kSoundGp);
    rr::sim::SoundMachine s{g, ram_.data(), *io_};
    rr::sim::SoundParams p;
    p.pitch = -1;
    p.volume = g.S32(0x800D6C0Cu);
    int32_t sound = 10;
    p.pan = 0x40;
    if (mode != 0) {
        p.pan = 0;
        rr::sim::GuestStartVoice(s, g.S32(kUiBank), 11, 0, 0, p);
        sound = 12;
        p.pan = 0x7F;
    }
    rr::sim::GuestStartVoice(s, g.S32(kUiBank), sound, 0, 0, p);
}

// SLUS 0x8001F5D4: the streamed voice `handle` stopped - released with ADSR2 0x1FC0 first when `fade`.
void ShellSound::StreamStop(uint32_t handle, int32_t fade) {
    GuestRam g(ram_.data(), rr::sim::kSoundGp);
    rr::sim::SoundMachine s{g, ram_.data(), *io_};
    for (uint32_t i = 0; i < 3u; ++i) {
        const uint32_t rec = kSys + 20u * i;
        if (g.U32(rec + 0x2Cu) != handle) continue;
        if (fade != 0) {
            rr::sim::SpuVoiceAttr a;
            a.mask = 0x40000;
            a.adsr2 = 0x1FC0;
            rr::sim::SpuSetVoiceAttr(s, g.U32(rec + 0x2Cu) >> 27, a);
        }
        rr::sim::StopVoice(s, handle);
        rr::sim::SpuSetReverbVoice(s, 1, 1u << (g.U32(rec + 0x2Cu) >> 27));
        g.W32(rec + 0x1Cu, 0);
        g.W32(rec + 0x2Cu, 0);
    }
}

// RASHCDF 0x8007F158. `jukebox`: the current screen is 47 (fe+0x00 == 0x2F), where the voices fade.
void ShellSound::MusicStop(int32_t flag, bool jukebox) {
    GuestRam g(ram_.data(), rr::sim::kSoundGp);
    if (g.U32(kPending) != 0) g.W32(kPending, 0);
    if (g.U32(kPlaying) != 0) {
        StreamStop(g.U32(kHandles), jukebox ? 1 : 0);
        StreamStop(g.U32(kHandles + 4u), jukebox ? 1 : 0);
    }
    g.W32(kPlaying, 0);
    if (g.U32(kFaded) != 0) {
        g.W32(kFaded, 0);
        g.W32(0x800D6C14u, g.U32(0x800D6C34u));
    }
    if (flag != 0) g.W32(kTrack, 0xFFFFFFFFu);
}

// RASHCDF 0x8007EDE0, with the loader 0x8007EEB8 for a track not loaded.
void ShellSound::MusicPlay(uint32_t track) {
    GuestRam g(ram_.data(), rr::sim::kSoundGp);
    if (track > 17u) return;
    if (g.U32(kTrack) == track) {
        if (g.U32(kPlaying) != 0) return;
        if (g.S32(kChunks) > 11) {
            rr::sim::SoundMachine s{g, ram_.data(), *io_};
            rr::sim::SoundParams p;
            p.pitch = 0x5CE;
            p.volume = g.S32(0x800D6C14u);
            p.pan = 0;
            g.W32(kHandles, rr::sim::StreamVoiceStart(s, 1, p, kSpuLeft));
            p.pan = 0x7F;
            g.W32(kHandles + 4u, rr::sim::StreamVoiceStart(s, 1, p, kSpuRight));
            rr::sim::KeyOnHandles(s, 2, kHandles);
            g.W32(kPlaying, 1);
            g.W32(kPending, 0);
            return;
        }
    } else { // 0x8007EEB8: a new track - 28 chunk reads from track * 0x70000 (0x8007EF64)
        g.W32(kIssued, 0);
        g.W32(kChunks, 0);
        MusicStop(1, false);
        g.W32(kOffset, track * kTrackBytes);
        g.W32(kLoading, 1);
        g.W32(kTrack, track);
        loading_ = true;
    }
    g.W32(kPending, 1);
}

// The 28 chunk reads complete (OURS: at once): 0x8007F028 moves each chunk's halves into SPU RAM, and
// 0x8007F0D0 counts them, asks MusicPlay again from the 12th on while one is pending, and clears the
// loading word at the 28th.
void ShellSound::Deliver() {
    if (!loading_) return;
    loading_ = false;
    GuestRam g(ram_.data(), rr::sim::kSoundGp);
    const uint32_t base = g.U32(kOffset);
    std::vector<uint8_t>& spu = spu_->Ram();
    for (uint32_t k = 0; k < 28u; ++k) {
        const size_t at = static_cast<size_t>(base) + kChunk * k;
        if (at + kChunk > album_.size()) break;
        std::memcpy(spu.data() + kSpuLeft + 0x2000u * k, album_.data() + at, 0x2000);
        std::memcpy(spu.data() + kSpuRight + 0x2000u * k, album_.data() + at + 0x2000, 0x2000);
        g.W32(kIssued, k + 1u);
        if (g.U32(kChunks) != k) continue;
        g.W32(kChunks, k + 1u);
        if (g.S32(kChunks) >= 12 && g.U32(kPending) != 0) MusicPlay(g.U32(kTrack));
        if (g.S32(kChunks) >= 28) g.W32(kLoading, 0);
    }
    g.W32(kOffset, base + kChunk * 28u);
}

void ShellSound::Frame(GuestRam& shell, const std::vector<uint32_t>& clicks, int musicRequest, bool musicStop,
                       rr::audio::Mixer& mixer) {
    if (!ready_) return;
    if (spuVoice_ == 0 || !mixer.IsPlaying(spuVoice_))
        spuVoice_ = mixer.Play(rr::audio::VoiceDesc{std::make_shared<rr::audio::SpuVoicesSource>(spu_)});
    GuestRam g(ram_.data(), rr::sim::kSoundGp);
    // The seven sliders, both copies (VolumeSet 0x8007F20C writes them in the shell arena).
    for (uint32_t a = 0x800D6C00u; a < 0x800D6C3Cu; a += 4u) g.W32(a, shell.U32(a));
    for (size_t i = 0; i < clicks.size(); ++i) {
        const uint32_t n = clicks[i];
        if (n == 0x8007EB1Cu && i + 2 < clicks.size()) {
            SliderPreview(static_cast<int32_t>(clicks[i + 1]), static_cast<int32_t>(clicks[i + 2]));
            i += 2;
        } else if (n == 0x8007ECFCu) {
            PreviewStop();
        } else if (n == 0x8007ED34u && i + 1 < clicks.size()) {
            SoundModeUi(static_cast<int32_t>(clicks[++i]));
        } else {
            PlayUiSound(n);
        }
    }
    if (musicStop) MusicStop(1, shell.S16(kFeCur) == 0x2F);
    if (musicRequest >= 0) MusicPlay(static_cast<uint32_t>(musicRequest));
    Deliver();
    rr::sim::SoundMachine s{g, ram_.data(), *io_};
    rr::sim::SoundService(s); // SLUS 0x8001EE94: key-offs, the voices' programming, key-ons
    const uint32_t t = g.U32(kTrack);
    track_ = t < 18u ? static_cast<int>(t) : -1;
    if (s.Faulted() && error_.empty()) error_ = "the shell's sound faulted";
}

void ShellSound::Stop(rr::audio::Mixer& mixer) {
    // The race replaces the shell's sound (its overlay and banks); coming back, the shell's sound init
    // 0x8007E824 runs again - so the state starts over here, voices and all.
    if (spuVoice_ != 0) mixer.Stop(spuVoice_);
    spuVoice_ = 0;
    if (ready_) Reset();
}

} // namespace rr::shell
