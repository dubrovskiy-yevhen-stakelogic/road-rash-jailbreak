// The two-player sound world's race-log line, read from the arena at the end.
// Header-only: rrgame's log reads it after the race.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#include "game/audio/mixer.h"
#include "game/audio/sound_runtime.h"
#include "game/mp_arena.h"

namespace rr::game {

inline std::string MpSoundLine(const SoundRuntime& snd, int sessionPlayers) {
    const uint32_t gs = snd.ArenaWord(0x8005B2F8u);
    const uint32_t players = gs != 0u ? snd.ArenaWord(gs + 0x30u) : 0u;
    constexpr uint32_t kGp = 0x8005AC8Cu;
    const uint32_t L = snd.ArenaWord(kGp + 1920u), E0 = snd.ArenaWord(kGp + 1952u);
    const uint32_t voices = snd.ArenaWord(0x800D6870u + 0x0Cu);
    std::string per;
    for (uint32_t p = 0; p < players && p < 2u && snd.Attached(); ++p) {
        const uint32_t E = E0 + 132u * p;
        int live = 0, left = 0, right = 0, both = 0; // by the louder side (UpdateVoice's pan law: pan 127 leaves L = V/64)
        for (const uint32_t off : {0x10u, 0x14u, 0x18u, 0x24u, 0x28u}) { // the five layers' handles (SoundRecordsInit)
            const uint32_t h = snd.ArenaWord(E + off);
            if (h == 0xFFFFFFFFu || h == 0u) continue;
            ++live;
            const uint32_t ch = snd.ArenaWord(voices + 44u * (h >> 27) + 0x1Cu); // the voice's SPU channel (+0x1C)
            if (ch >= 24u || !snd.Spu()->State(static_cast<int>(ch)).on) continue;
            const uint16_t vl = snd.Spu()->Read(ch * 0x10u), vr = snd.Spu()->Read(ch * 0x10u + 2u);
            const uint32_t l = vl & 0x7FFFu, r = vr & 0x7FFFu;
            if (l > 4u * r) ++left;
            else if (r > 4u * l) ++right;
            else if (l != 0u) ++both;
        }
        char b[260];
        std::snprintf(b, sizeof(b), "; player %u: engine record 0x%08X bike 0x%08X started %u, %d layer handle(s), sounding on "
                      "the SPU %d left / %d right / %d centred (louder side x4), listener +0x14 (pan) %u",
                      p + 1u, E, snd.ArenaWord(E), snd.ArenaWord(E + 4u) & 0xFFu, live, left, right, both,
                      snd.ArenaWord(L + 72u * p + 0x14u));
        per += b;
    }
    const MpSoundCounts& c = MpSoundCounters();
    if (!snd.Attached()) {
        char n[300];
        std::snprintf(n, sizeof(n),
                      "mpsound: %u player(s), sound world NOT attached (no free 16 KiB block; RRJB_MPARENA=off "
                      "puts a two-player stat array in the last one) - no AudioFrame, no countdown voice, no speech\n",
                      static_cast<unsigned>(sessionPlayers));
        return n;
    }
    char b[900];
    std::snprintf(b, sizeof(b),
                  "mpsound: %u player(s), sound world %s, stat array *(0x8005B248) = 0x%08X (%s), "
                  "countdown voice 0x800164B4 handle 0x%08X, music %s, RoadNote effects on player 2's bike %zu burst(s) / "
                  "%zu spray(s)%s\n",
                  players, snd.Attached() ? "ATTACHED" : "NOT attached", snd.ArenaWord(0x8005B248u),
                  snd.ArenaWord(0x8005B248u) == StatArrayAt(players == 2u ? 2u : 1u) ? "BuildGrid's order, mp_arena.h"
                                                                                  : "OURS: a cell buffer",
                  snd.CountdownVoiceStarted(),
                  players == 1u ? "one player: the album" : "none (the original starts it only with one player)",
                  c.p2Bursts, c.p2Sprays, per.c_str());
    return b;
}

} // namespace rr::game
