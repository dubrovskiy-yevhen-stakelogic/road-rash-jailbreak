// The strike, the pedestrian's voice and the race-over signal in the product - see strike_product.h.
#include "game/strike_product.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "game/audio/sound_runtime.h"

namespace rr::game {

bool StrikeOn() {
    static const bool on = [] {
        const char* v = std::getenv("RRJB_STRIKE");
        return !(v != nullptr && std::strcmp(v, "off") == 0);
    }();
    return on;
}

StrikeCounts& StrikeCounters() {
    static StrikeCounts c;
    return c;
}

void ProductRaceOverSignal(SoundRuntime& s, uint32_t pause, const std::function<void(const std::string&)>& seam,
                           const char* offSeam) {
    if (!StrikeOn()) {
        if (seam) seam(offSeam);
        return;
    }
    StrikeCounts& c = StrikeCounters();
    ++(pause != 0 ? c.overPause : c.overResume);
    const bool wasPaused = s.Attached() && s.ArenaWord(0x8005AC8Cu + 1960u) != 0;
    if (!s.RaceOverSignal(pause)) { // PORTED, row `race_over_signal`
        ++c.overRefused;
        if (seam) seam("SLUS 0x80018C1C RaceOverSignal (PORTED) was not run: no sound world attached, or it faulted");
        return;
    }
    if (pause != 0 && !wasPaused) {
        ++c.overPausedTaken;
        if (!c.haveSliders) {
            c.haveSliders = true;
            for (uint32_t k = 0; k < 6; ++k) c.slidersAfterPause[k] = s.ArenaWord(0x800D6C00u + 4u * k);
        }
    }
}

std::string StrikeTotals() {
    const StrikeCounts& c = StrikeCounters();
    char b[900];
    std::snprintf(b, sizeof(b),
                  "the strike port%s: Strike RASHCDG 0x800C1370 (op 9) %zu call(s), %zu refused: FightStep branch %zu, "
                  "FightRestart branch %zu, reach tests %zu, hits (ApplyHit) %zu, lean stance events %zu, riders hurt %zu, "
                  "knocked to 0 %zu; PedVoice SLUS 0x8001B44C (recovery domain) %zu call(s), %zu sound(s); "
                  "RaceOverSignal SLUS 0x80018C1C pause %zu (took effect %zu), resume %zu, refused %zu; sliders after "
                  "the pause %s%u %u %u %u %u %u\n",
                  StrikeOn() ? "" : " OFF (RRJB_STRIKE=off)", c.calls, c.refused, c.stepped, c.restarted, c.armed,
                  c.hits, c.leans, c.hpDrops, c.knockOffs, c.pedVoices, c.pedVoiced, c.overPause, c.overPausedTaken,
                  c.overResume, c.overRefused, c.haveSliders ? "" : "(none) ", c.slidersAfterPause[0],
                  c.slidersAfterPause[1], c.slidersAfterPause[2], c.slidersAfterPause[3], c.slidersAfterPause[4],
                  c.slidersAfterPause[5]);
    return b;
}

} // namespace rr::game
