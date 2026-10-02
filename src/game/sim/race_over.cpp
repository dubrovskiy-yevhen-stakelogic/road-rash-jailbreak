// SLUS 0x80018C1C RaceOverSignal, ported from our own disassembly of SLUS_010.53 - see race_over.h.
#include "game/sim/race_over.h"

#include "game/sim/sound_frame.h"

namespace rr::sim {
namespace {
constexpr uint32_t kSliders = 0x800D6C00, kSaved = 0x800D6C20, kCueVoice = 0x800D6BC0;
constexpr uint32_t kTwoUpFlag = 0x800CDAC0, kGsPtr = 0x8005B2F8;
constexpr uint32_t kVoiceGp = 1964, kMuteGp = 1960, kListenerAtGp = 1920;
} // namespace

void RaceOverSignal(SoundMachine& s, uint32_t pause) {
    GuestRam& g = s.m;
    const uint32_t gp = kSoundGp;
    SoundParams p;
    p.pan = 64;                                                              // sw v0(64),24(sp)
    p.volume = static_cast<int32_t>(g.U32(kSliders + 12u) * 127u) >> 7;     // sll 7; subu; sra 7
    if (pause == 0) {                                                        // 0x80018D44: resume
        p.pitch = 1486;
        const uint32_t v0 = g.U32(kSaved), v4 = g.U32(kSaved + 4u), v8 = g.U32(kSaved + 8u);
        const uint32_t v12 = g.U32(kSaved + 12u), v20 = g.U32(kSaved + 20u);
        const uint32_t voice = g.U32(gp + kVoiceGp);
        g.W32(kSliders, v0);
        g.W32(kSliders + 4u, v4);
        g.W32(kSliders + 8u, v8);
        g.W32(kSliders + 12u, v12);
        g.W32(kSliders + 20u, v20);
        if (voice != 0) UpdateVoice(s, voice, p);                            // 0x80018D88 -> 0x8001F6A4
        const uint32_t cue = g.U32(kCueVoice);
        if (cue != 0) {
            p.pitch = 743;
            UpdateVoice(s, cue, p);                                          // 0x80018DA8
        }
        g.W32(gp + kMuteGp, 0);
        return;
    }
    if (g.U32(gp + kMuteGp) != 0) return;                                    // already paused
    SetReverbDepth(s, 0, 0);                                                 // 0x80018C64 -> 0x8001F054
    const uint32_t L = g.U32(gp + kListenerAtGp);
    g.W32(L + 24u, 0);
    if (g.U32(g.U32(kGsPtr) + 48u) == 2u) g.W32(L + 96u, 0);
    g.W32(kSaved, g.U32(kSliders));
    const uint32_t s8 = g.U32(kSliders + 8u), s12 = g.U32(kSliders + 12u), s20 = g.U32(kSliders + 20u);
    const uint32_t twoUp = g.U32(kTwoUpFlag);
    g.W32(kSaved + 8u, s8);
    g.W32(kSaved + 12u, s12);
    g.W32(kSaved + 20u, s20);
    if (twoUp != 2u || (g.U8(g.U32(kGsPtr) + 4u) & 1u) == 0) g.W32(kSaved + 4u, g.U32(kSliders + 4u));
    const uint32_t voice = g.U32(gp + kVoiceGp);
    g.W32(kSliders, 0);
    g.W32(kSliders + 4u, 0);
    g.W32(kSliders + 8u, 0);
    p.pitch = 0;
    if (voice != 0) UpdateVoice(s, voice, p);                                // 0x80018D10
    const uint32_t cue = g.U32(kCueVoice);
    if (cue != 0) UpdateVoice(s, cue, p);                                    // 0x80018D2C
    g.W32(gp + kMuteGp, 1);
}

} // namespace rr::sim
