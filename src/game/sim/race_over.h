#pragma once
// SLUS 0x80018C1C RaceOverSignal(pause) - the audio pause / resume the race loop, the pad poll and the
// camera director call. Ported from our own disassembly of the player's own executable:
//
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//
// and accepted only by the `rrverify phys` row `race_over_signal` (tools\rrverify\rows_strike.inc).
// Memory model: sound_engine.h's SoundMachine (a guest-address view; the libspu arms it reaches write
// the SPU through its SoundIo).
//
// 0x80018C1C, frame 48. SoundParams {pitch, volume = slider(0x800D6C0C) * 127 >> 7 as it is AT ENTRY,
// pan 64} on its own stack.
//   pause (a0 != 0), only when gp+1960 is 0: SetReverbDepth(0, 0) (0x8001F054); the listener record
//     *(gp+1920) +0x18 = 0 (and +0x60, the second player's, with two players); the sliders +0, +8, +12,
//     +20 of 0x800D6C00 are saved to 0x800D6C20.. (+4 too, unless *(0x800CDAC0) == 2 and game_state+4
//     bit 0); sliders +0, +4, +8 = 0; UpdateVoice(pitch 0) on the voice handle gp+1964 and on the cue
//     voice *(0x800D6BC0) when they are not 0; gp+1960 = 1.
//   resume (a0 == 0), unconditionally: sliders +0, +4, +8, +12, +20 <- the saved copy; UpdateVoice with
//     pitch 0x5CE on gp+1964's handle and 0x2E7 on the cue voice (when not 0); gp+1960 = 0.
#include <cstdint>

#include "game/sim/sound_engine.h"

namespace rr::sim {

constexpr uint32_t kRaceOverSignalFn = 0x80018C1C;

void RaceOverSignal(SoundMachine& s, uint32_t pause);

} // namespace rr::sim
