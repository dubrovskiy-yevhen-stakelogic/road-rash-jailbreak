#pragma once
// Three seams of the race run by PORTED code in the product.
//   * RASHCDG 0x800C1370 Strike - op 9's strike in the AI command pass (sim\strike.h), run by
//     RaceSession::StrikeCommand (fight_session.cpp) on the fight code's product callees;
//   * SLUS 0x8001B44C PedVoice - the pedestrian pass's call in the rider-recovery domain runs the
//     PORTED sim\partners.h one (recover_race.cpp) - its pedestrian call site passes flag 0
//     (RASHCDG 0x800CAD28 `move a3,zero`), so the original plays nothing there, and neither do we;
//   * SLUS 0x80018C1C RaceOverSignal - the race loop's audio pause at the race's end, the pad poll's
//     resume on the first race frame and the camera director's resume (sim\race_over.h), run on the
//     sound runtime's world (the race arena), whose ported engine note, road layer and reverb read the
//     sliders and flags it writes.
// RRJB_STRIKE=off: the negative control - the three calls go back to their named seams (no effect).
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace rr::game {

class SoundRuntime;

bool StrikeOn();

struct StrikeCounts {
    size_t calls = 0, refused = 0;              // Strike: calls, refused (arena restored)
    size_t stepped = 0, restarted = 0;          // its FightStep / FightRestart branch
    size_t armed = 0, hits = 0, leans = 0;      // the reach test ran / ApplyHit ran / the stance event 16
    size_t hpDrops = 0, knockOffs = 0;          // riders whose health went down in a Strike call / to 0
    size_t pedVoices = 0, pedVoiced = 0;        // PedVoice calls in the recovery domain / sounds played
    size_t overPause = 0, overResume = 0, overRefused = 0, overPausedTaken = 0; // RaceOverSignal
    bool haveSliders = false;
    uint32_t slidersAfterPause[6] = {};         // 0x800D6C00.. after the first pause that took effect
};
StrikeCounts& StrikeCounters();
std::string StrikeTotals();

// SLUS 0x80018C1C(pause) on the session's sound world. `seam` names a refusal or, with RRJB_STRIKE=off,
// the call not run (`offSeam`).
void ProductRaceOverSignal(SoundRuntime& s, uint32_t pause, const std::function<void(const std::string&)>& seam,
                           const char* offSeam);

} // namespace rr::game
