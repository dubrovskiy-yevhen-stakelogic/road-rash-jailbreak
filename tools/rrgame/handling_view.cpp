// The head view's drawn lean and view roll (handling_view.h).
#include "handling_view.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace rrgame {

namespace {
constexpr float kDeg = 57.29577951f;
}

float DrawnLeanDegrees(const VisualLean& lean) { return lean.on ? lean.rollDegrees + lean.angle * kDeg : lean.rollDegrees; }

float ViewRollDegrees(const float fwd0[3], const float up0[3]) {
    const auto len = [](const float a[3]) { return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); };
    float fwd[3] = {fwd0[0], fwd0[1], fwd0[2]}, up[3] = {up0[0], up0[1], up0[2]};
    const float lf = len(fwd), lu = len(up);
    if (!(lf > 1e-9f) || !(lu > 1e-9f)) return 0.0f;
    for (int k = 0; k < 3; ++k) {
        fwd[k] /= lf;
        up[k] /= lu;
    }
    // the same construction as vr_visual_lean.h From: the world's up (-y) made square to forward, the signed angle to up
    float lvl[3] = {0.0f, -1.0f, 0.0f};
    const float wf = lvl[1] * fwd[1];
    for (int k = 0; k < 3; ++k) lvl[k] -= fwd[k] * wf;
    const float ll = len(lvl);
    if (!(ll > 1e-4f)) return 0.0f;
    for (float& c : lvl) c /= ll;
    const float cr[3] = {lvl[1] * up[2] - lvl[2] * up[1], lvl[2] * up[0] - lvl[0] * up[2], lvl[0] * up[1] - lvl[1] * up[0]};
    return std::atan2(cr[0] * fwd[0] + cr[1] * fwd[1] + cr[2] * fwd[2], lvl[0] * up[0] + lvl[1] * up[1] + lvl[2] * up[2]) * kDeg;
}

void HandlingViewMeter::Note(long frame, bool vr, const VisualLean& lean, const float viewFwd[3], const float viewUp[3]) {
    if (frame == lastFrame_) return; // once a game frame (a VR frame repeated between the steps, the desktop's shot)
    lastFrame_ = frame;
    ++frames_;
    vr_ = vr;
    const double game = lean.rollDegrees, drawn = DrawnLeanDegrees(lean), view = ViewRollDegrees(viewFwd, viewUp);
    maxGame_ = std::max(maxGame_, std::fabs(game));
    maxDrawn_ = std::max(maxDrawn_, std::fabs(drawn));
    maxView_ = std::max(maxView_, std::fabs(view));
    if (std::fabs(drawn) >= 10.0) { // the view's share of the lean where there is a lean to share
        const double r = view / drawn;
        sumRatio_ += r;
        ++ratioFrames_;
    }
    if (csv_ == nullptr) {
        static bool tried = false;
        const char* path = std::getenv("RRJB_HANDLING_VIEW_LOG");
        if (!tried && path != nullptr && *path != '\0') {
            tried = true;
            csv_ = std::fopen(path, "w");
            if (csv_) std::fprintf(static_cast<std::FILE*>(csv_), "frame,vr,game_roll_deg,drawn_lean_deg,view_roll_deg\n");
        }
    }
    if (csv_) std::fprintf(static_cast<std::FILE*>(csv_), "%ld,%d,%.3f,%.3f,%.3f\n", frame, vr ? 1 : 0, game, drawn, view);
}

std::string HandlingViewMeter::Summary() const {
    if (frames_ == 0) return "";
    if (csv_) std::fflush(static_cast<std::FILE*>(csv_));
    char b[400];
    std::snprintf(b, sizeof(b),
                  "handling view: %ld %s head-view frame(s) - the game's roll up to %.1f deg, the bike drawn "
                  "leaning up to %.1f deg, the view rolled up to %.1f deg (%.0f %% of the drawn lean on average over %ld "
                  "leaning frame(s))",
                  frames_, vr_ ? "VR" : "desktop", maxGame_, maxDrawn_, maxView_,
                  ratioFrames_ ? 100.0 * sumRatio_ / static_cast<double>(ratioFrames_) : 0.0, ratioFrames_);
    return b;
}

HandlingViewMeter& HandlingView() {
    static HandlingViewMeter m;
    return m;
}

} // namespace rrgame
