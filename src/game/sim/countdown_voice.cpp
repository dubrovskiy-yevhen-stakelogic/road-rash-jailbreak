#include "game/sim/countdown_voice.h"

namespace rr::sim {

// SLUS 0x800164B4..0x80016524.
uint32_t CountdownVoiceStart(SoundMachine& s) {
    GuestRam& g = s.m;
    if (g.U32(g.U32(0x8005B2F8u) + 0x30u) != 2u) return 0;       // bne v1,2
    const int32_t v = g.S32(0x800D6C0Cu);                         // slider [3], the 3D master
    SoundParams p;
    p.pitch = -1;                                                 // sp+24
    p.volume = static_cast<int32_t>((static_cast<uint32_t>(v) << 7) - static_cast<uint32_t>(v)) >> 7; // sp+28
    p.pan = 0x40;                                                 // sp+32
    const uint32_t h = GuestStartVoice(s, g.S32(g.gp() + 1912u), 110, 0, 1, p);
    g.W32(g.gp() + kCountdownVoiceGp, h);
    return h;
}

// SLUS 0x80016528..0x80016548.
void CountdownVoiceStop(SoundMachine& s) {
    GuestRam& g = s.m;
    StopVoice(s, g.U32(g.gp() + kCountdownVoiceGp));
    g.W32(g.gp() + kCountdownVoiceGp, 0);
}

} // namespace rr::sim
