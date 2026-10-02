#pragma once
// The countdown's voice, transcribed from our own disassembly of
//
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text 0x80010000
//
// SLUS 0x800164B4 (0x80011BF8, the race start after EngineSetup, when game_state+0x30 != 1): in a
// TWO-player race only (game_state+0x30 == 2) sound 110 of the default bank gp+1912 is started -
// StartVoice(bank, 110, 0, 1, {pitch -1, volume slider[3] * 127 >> 7, pan 0x40}) - and its handle kept
// at gp+1964. In a one-player race the loader's gp+1964 = 0 (RASHCDI 0x80062D34) stays.
// SLUS 0x80016528 (the HUD's countdown end, HudFrame RASHCDG 0x8005E848): StopVoice(gp+1964), then
// gp+1964 = 0 - including StopVoice(0), which stops voice 0 when its serial is 0.
#include <cstdint>

#include "game/sim/sound_engine.h"

namespace rr::sim {

constexpr uint32_t kCountdownVoiceStartFn = 0x800164B4; // 116 B, frame 48
constexpr uint32_t kCountdownVoiceStopFn = 0x80016528;  // 36 B, frame 24
constexpr uint32_t kCountdownVoiceGp = 1964;

// Returns the handle stored (0 when nothing was started: not a two-player race).
uint32_t CountdownVoiceStart(SoundMachine& s);
void CountdownVoiceStop(SoundMachine& s);

} // namespace rr::sim
