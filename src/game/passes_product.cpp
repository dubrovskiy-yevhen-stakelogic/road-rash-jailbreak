#include "game/passes_product.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rr::game {

bool PassesWhole() {
    static const bool whole = [] {
        const char* v = std::getenv("RRJB_PASSES");
        return v == nullptr || std::strcmp(v, "session") != 0;
    }();
    return whole;
}

PassTotals& PassRunTotals() {
    static PassTotals t;
    return t;
}

std::string PassTotalsLine() {
    const PassTotals& t = PassRunTotals();
    char b[1200];
    if (!PassesWhole()) {
        std::snprintf(b, sizeof(b),
                      "the world passes SWITCHED OFF (RRJB_PASSES=session, the negative control): the session's own "
                      "ordering of the world pass's children %zu frame(s) and of the rider / engine pass's %zu; the "
                      "rider pass's class-list walk [0x8007C9DC, 0x8007DCD4) never runs\n",
                      t.sessionWorld, t.sessionEngine);
        return b;
    }
    std::snprintf(b, sizeof(b),
                  "the world passes (passes.h): the PORTED WorldBikePass 0x8008AC80 %zu frame(s), "
                  "RiderEnginePass 0x8008ACE8 %zu, RiderPass 0x8007B840 whole %zu (refused %zu); its class-list walk "
                  "[0x8007C9DC, 0x8007DCD4) moved %zu bike-frame(s) in %zu frame(s) (refused %zu); the PORTED "
                  "ActivationPass 0x80093E6C %zu time(s) (%zu refused and restored whole), DownedRiderPass 0x800950E8 "
                  "%zu (%zu refused); HazardPass 0x800A13C4 %zu (%zu refused); the frame's clock (+0x0C, the PORTED pad "
                  "poll clock region 0x8001CD00 and FrameDelta 0x8001C428) %zu frame(s), main's first race frame %zu, "
                  "the PORTED ResultsPrepare 0x8003F708 %zu time(s)\n",
                  t.worldPasses, t.engineWholePasses, t.riderPasses, t.riderPassRefused, t.classBikes, t.classFrames,
                  t.classRefused, t.activationPasses, t.activationRefused, t.downedPasses, t.downedRefused,
                  t.hazardPasses, t.hazardRefused, t.clockFrames, t.firstFrames, t.resultsPrepared);
    return b;
}

} // namespace rr::game
