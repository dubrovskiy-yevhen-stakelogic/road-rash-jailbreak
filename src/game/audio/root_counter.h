#pragma once
// Root counter 2 as the console would read it at a given moment.
//
// The console's counter, from our own disassembly of SLUS_010.53 (SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1):
// the sound start-up SLUS 0x8001DFE4 (called from the boot 0x800117BC) opens the counter's event and calls
// SetRCnt 0x80043E64(0xF2000002, 0xFFFF, 0x1000): for counter 2 without bit 0 of the mode argument that
// writes the mode word 0x248 | 0x10 = 0x258 - bit 9 the source "system clock / 8", bit 3 reset at the
// target, bits 4 / 6 the repeated target interrupt (the sound tick 0x8001DF64) - and the target 0xFFFF;
// StartRCnt 0x80043F9C and the interrupt enable follow. So the counter runs at 33,868,800 / 8 = 4,233,600
// Hz from power-on, 0..0xFFFF (a lap of 15.48 ms), and GetRCnt 0x80043F00 reads its current value.
//
// What reads it at a race start: SpeechInit 0x8001A424 (the first AUDTAUNT.STR record of the race,
// `(GetRCnt(2) & 0xFF) * 133 >> 8`) and MusicPickShuffle 0x80024B20 (the first track tried, `18 *
// (GetRCnt(2) & 0xFF) >> 8`); the low byte laps every 60 us, so on the console what the player gets is
// set by how long the boot, the menus and the CD loads took - different every race.
//
// The product: the same counter over the time since this process started (the console's since power-on),
// read from the steady clock (OURS: the host's clock stands in for the console's cycles; the unit, the
// lap and the wrap are the console's). A SCRIPTED run keeps the fixed value 0, so two runs of one command
// stay equal (the gates compare them).
#include <chrono>
#include <cstdint>

namespace rr::game {

// The process's power-on: the steady clock at static initialisation (before main - a function-local static
// would start the clock at the first read, and every race would read about the same count).
inline const std::chrono::steady_clock::time_point kConsolePowerOn = std::chrono::steady_clock::now();
inline std::chrono::steady_clock::time_point ConsolePowerOn() { return kConsolePowerOn; }

// True for an interactive run (rrgame sets it when no frame limit is given).
inline bool& LiveRootCounter() {
    static bool live = false;
    return live;
}

// Root counter 2 after `microseconds` since power-on: 4,233,600 counts a second, 0..0xFFFF.
inline uint16_t RootCounter2At(uint64_t microseconds) {
    const uint64_t counts = (microseconds / 1000000u) * 4233600u + (microseconds % 1000000u) * 4233600u / 1000000u;
    return static_cast<uint16_t>(counts & 0xFFFFu);
}

// Its value now (a live run), or 0 (a scripted run: fixed).
inline uint16_t ConsoleRootCounter2() {
    if (!LiveRootCounter()) return 0;
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                                          ConsolePowerOn())
                        .count();
    return RootCounter2At(static_cast<uint64_t>(us < 0 ? 0 : us));
}

} // namespace rr::game
