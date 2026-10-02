// The wheelie's picture and the VR gesture (vr_wheelie.h). OURS.
#include "vr_wheelie.h"

#include "game/handling_modern.h" // Handling(): the wheelie settings
#include "game/wheelie.h"
#include "vr_handlebars.h"
#include "vr_horizon.h" // ViewPitch::RemovedWheelie

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace rrgame {

namespace {

inline bool InRam(uint32_t a) { return a >= 0x80000000u && a < 0x801FFFF0u; }

rr::render::Mat4 BikeModel(const uint8_t* ram, uint32_t bike) {
    rr::render::Mat4 m;
    for (uint32_t c = 0; c < 3; ++c) {
        for (uint32_t k = 0; k < 3; ++k) {
            int16_t v = 0;
            std::memcpy(&v, ram + ((bike + 0x1B0u + 6u * c + 2u * k) & 0x1FFFFFu), 2);
            m.m[4 * c + k] = static_cast<float>(v) / 4096.0f;
        }
        m.m[4 * c + 3] = 0.0f;
    }
    for (uint32_t k = 0; k < 3; ++k) {
        int32_t v = 0;
        std::memcpy(&v, ram + ((bike + 0xB8u + 4u * k) & 0x1FFFFFu), 4);
        m.m[12 + k] = static_cast<float>(static_cast<double>(v) / 65536.0);
    }
    m.m[15] = 1.0f;
    return m;
}

// A lift's distribution in 1 mm bins from -0.3 m to +0.5 m (the ends take what is past them).
struct LiftHistogram {
    static constexpr int kBins = 800;
    long bins[kBins] = {};
    long count = 0;
    float max = -1.0f;
    void Add(float metres) {
        const int i = std::clamp(static_cast<int>(std::floor((metres + 0.3f) * 1000.0f)), 0, kBins - 1);
        ++bins[i];
        ++count;
        max = count == 1 ? metres : std::max(max, metres);
    }
    float Percentile(double q) const { // the bin's upper edge
        if (count == 0) return 0.0f;
        const long want = static_cast<long>(std::ceil(std::clamp(q, 0.0, 1.0) * static_cast<double>(count)));
        long seen = 0;
        for (int i = 0; i < kBins; ++i) {
            seen += bins[i];
            if (seen >= std::max(1L, want)) return std::min(max, static_cast<float>(i + 1) / 1000.0f - 0.3f);
        }
        return max;
    }
};

// The Handlebars gesture's state and its meters (WheelieBarsInput).
struct BarsGesture {
    long lastFrame = -1;
    bool both = false;          // both hands on the bars last frame
    float base[2] = {0, 0};     // each hand's height over its drawn grip when at rest (the bike's up, metres)
    float settle = 0.0f;        // seconds since the two-handed grab (the base follows quickly while it settles)
    bool latched = false;       // a wheelie under way: the lift measured in the seat's space from the latch
    float roomRef = 0.0f;       // ... the hands' mean seat-space height the lift is measured from
    // the meters
    long frames = 0, grabs = 0, wantFrames = 0, keepFrames = 0, latches = 0;
    float maxRoomLift = 0.0f, maxKeep = 0.0f, maxLift = 0.0f;
    LiftHistogram restLow, restMean; // not in a wheelie, settled: the lower hand's lift, the pair's mean (metres)
    // the previous gesture replayed as a meter (both hands' mean seat-space rise, half the pull toward the rider, from
    // the grab with a 4 s drift while not pulling; 4 cm dead zone, 18 cm full): what it would have asked for
    bool oldRef = false;
    float oldY = 0.0f, oldZ = 0.0f, oldMax = 0.0f, oldMaxRaise = 0.0f;
    long oldFrames = 0, oldPulls = 0;
};
BarsGesture& Gesture() {
    static BarsGesture g;
    return g;
}


struct ViewMeter { // the VR head view in a wheelie (WheelieViewComfort)
    long frames = 0, pitched = 0;
    double maxDrawn = 0.0, maxView = 0.0; // the drawn pitch; the view's own pitch by it (its elevation over the unpitched)
};
ViewMeter& View() {
    static ViewMeter v;
    return v;
}
float Elevation(const float f[3]) { // degrees above the level (the world's Y points down)
    const float l = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    return l > 1e-6f ? std::asin(std::clamp(-f[1] / l, -1.0f, 1.0f)) * 57.29578f : 0.0f;
}

} // namespace

// what this frame's ViewPitch (vr_horizon.h, the VR head view) took out of the drawn rows because the held
// wheelie covers it - the layer draws it back, so the whole held pitch is drawn and the view keeps its share of all of it
static float LayerPitch(uint32_t bike) {
    float pitch = 0.0f;
    if (!rr::game::PlayerWheelie().DrawnPitch(pitch)) pitch = 0.0f;
    return pitch + ProductViewPitch().RemovedWheelie(bike);
}

void AddWheeliePitch(VisualLean& lean, const rr::render::Mat4& model, const uint8_t* ram, uint32_t bike, float contactUp) {
    if (ram == nullptr || !InRam(bike)) return;
    const float pitch = LayerPitch(bike);
    if (!(pitch > 0.0f)) return;
    // the rear wheel's contact point: 0.75 of the box's half length (+0x134, the half extent along the tangent,
    // the collision box) behind the origin, down at the contact line
    int32_t half = 0;
    std::memcpy(&half, ram + ((bike + 0x134u) & 0x1FFFFFu), 4);
    float rearBack = static_cast<float>(half) / 65536.0f * 0.75f;
    if (!(rearBack > 0.1f && rearBack < 4.0f)) rearBack = 0.75f;
    if (!lean.on) lean = VisualLean::From(model, 1.0f, contactUp); // (the roll measured for the logs, no turn)
    lean.AddPitch(model, pitch, contactUp, rearBack);
}

void AddWheeliePitch(VisualLean& lean, const uint8_t* ram, uint32_t bike, float contactUp) {
    if (ram == nullptr || !InRam(bike) || !(LayerPitch(bike) > 0.0f)) return;
    AddWheeliePitch(lean, BikeModel(ram, bike), ram, bike, contactUp);
}

void WheelieViewComfort(const VisualLean& lean, float fwd[3], float up[3]) {
    ViewMeter& v = View();
    ++v.frames;
    if (!lean.on || !lean.pitchOn) return;
    float unpitched[3];
    VisualLean::Rotate(lean.pitchAxis, -lean.pitchAngle, fwd, unpitched);
    const float back = -(1.0f - rr::game::PlayerWheelie().ViewPitchKeep()) * lean.pitchAngle;
    VisualLean::Rotate(lean.pitchAxis, back, fwd, fwd);
    VisualLean::Rotate(lean.pitchAxis, back, up, up);
    ++v.pitched;
    v.maxDrawn = std::max(v.maxDrawn, static_cast<double>(std::fabs(lean.pitchAngle)) * 57.29578);
    v.maxView = std::max(v.maxView, static_cast<double>(Elevation(fwd) - Elevation(unpitched)));
}

bool WheelieBarsInput(const VrHandlebars& bars, long frame, bool& want, float& keep, float* lowOut, float* meanOut,
                      float* legacyOut) {
    BarsGesture& g = Gesture();
    want = false;
    keep = 0.0f;
    VrHandlebars::HandView hv[2];
    const bool both = bars.Hand(0, hv[0]) && hv[0].grabbed && bars.Hand(1, hv[1]) && hv[1].grabbed && hv[0].bikeValid &&
                      hv[1].bikeValid;
    const float dt = g.lastFrame >= 0 && frame > g.lastFrame ? std::min(0.1f, static_cast<float>(frame - g.lastFrame) / 60.0f) : 0.0f;
    g.lastFrame = frame;
    if (!both) {
        g.both = g.latched = g.oldRef = false;
        return false;
    }
    const rr::game::WheelieSettings& ws = rr::game::Handling().wheelie;
    const float start = ws.LiftStart(), full = ws.LiftFull(), release = ws.LiftRelease();
    const bool engaged = rr::game::PlayerWheelie().InputEngaged(); // (the gate as the last frame left it)
    // each hand's height over the grip it holds, along the drawn bike's up: the bike's lean, pitch and shake move the
    // grips and the eye together, so a hand that stays on its grip reads 0 whatever the bike does
    float rel[2];
    for (int h = 0; h < 2; ++h) rel[h] = hv[h].bike[1] - hv[h].gripBike[1];
    if (!g.both) { // a two-handed grab: the rest height taken now, settled over kSettle
        g.both = true;
        g.settle = 0.0f;
        for (int h = 0; h < 2; ++h) g.base[h] = rel[h];
        ++g.grabs;
    }
    g.settle += dt;
    constexpr float kSettle = 0.6f; // s: the hands finding their hold after the grab
    const bool settled = g.settle >= kSettle;
    float lift[2];
    for (int h = 0; h < 2; ++h) {
        lift[h] = rel[h] - g.base[h];
        if (!settled) { // settling: the rest height follows quickly both ways
            g.base[h] += lift[h] * std::min(1.0f, dt / 0.15f);
        } else if (!engaged) {
            // riding: down quickly (a hand sinking is never a lift), up only slowly and only for small offsets (a
            // player shifting on the seat); a hand on its way up past a third of the start is left alone
            if (lift[h] < 0.0f) g.base[h] += lift[h] * std::min(1.0f, dt / 0.4f);
            else if (lift[h] < start / 3.0f) g.base[h] += lift[h] * std::min(1.0f, dt / 6.0f);
        }
        lift[h] = rel[h] - g.base[h];
    }
    const float low = std::min(lift[0], lift[1]), mean = 0.5f * (lift[0] + lift[1]);
    if (lowOut != nullptr) *lowOut = low;
    if (meanOut != nullptr) *meanOut = mean;
    const float roomY = 0.5f * (hv[0].local[1] + hv[1].local[1]);
    ++g.frames;
    if (settled && !engaged && rr::game::PlayerWheelie().RidingOnGround()) {
        g.restLow.Add(low);
        g.restMean.Add(mean);
        g.maxLift = std::max(g.maxLift, mean);
    }
    // the start: both hands up together - each at least three quarters of the start, their mean the start
    want = settled && low >= 0.75f * start && mean >= start;
    if (want) ++g.wantFrames;
    if (engaged) {
        // under way: the lift held in the seat's space from the latch (the pitched bike carries the grips up toward
        // the player - measured against them the lift would shrink as the front rises and let go of itself)
        if (!g.latched) {
            g.latched = true;
            g.roomRef = roomY - mean;
            ++g.latches;
        }
        const float roomLift = roomY - g.roomRef;
        g.maxRoomLift = std::max(g.maxRoomLift, roomLift);
        keep = std::clamp((roomLift - release) / (full - release), 0.0f, 1.0f);
    } else {
        g.latched = false;
        keep = std::clamp((mean - release) / (full - release), 0.0f, 1.0f);
    }
    if (engaged && keep > 0.0f) ++g.keepFrames;
    g.maxKeep = std::max(g.maxKeep, engaged ? keep : 0.0f);
    {   // the previous gesture, replayed (a meter only)
        const float y = roomY, z = 0.5f * (hv[0].local[2] + hv[1].local[2]);
        if (!g.oldRef) {
            g.oldRef = true;
            g.oldY = y;
            g.oldZ = z;
        }
        const float raise = (y - g.oldY) + 0.5f * std::max(0.0f, z - g.oldZ);
        const float pull = std::clamp((raise - 0.04f) / 0.14f, 0.0f, 1.0f);
        if (pull < 0.1f && dt > 0.0f) {
            const float k = std::min(1.0f, dt / 4.0f);
            g.oldY += (y - g.oldY) * k;
            g.oldZ += (z - g.oldZ) * k;
        }
        ++g.oldFrames;
        if (legacyOut != nullptr) *legacyOut = pull;
        if (pull > 0.0f && !engaged) ++g.oldPulls;
        if (!engaged) {
            g.oldMax = std::max(g.oldMax, pull);
            g.oldMaxRaise = std::max(g.oldMaxRaise, raise);
        }
    }
    return true;
}

float WheelieStickBack(bool connected, uint8_t ly) {
    if (!connected) return 0.0f;
    return std::clamp(static_cast<float>(static_cast<int>(ly) - 0x80) / 127.0f, 0.0f, 1.0f); // 0xFF: full back / down
}

std::string WheelieBarsTotals() {
    const BarsGesture& g = Gesture();
    const ViewMeter& v = View();
    std::string s;
    char b[320];
    if (g.frames > 0) {
        const rr::game::WheelieSettings& ws = rr::game::Handling().wheelie;
        char w[900];
        std::snprintf(w, sizeof(w),
                      "vr wheelie: both hands on the bars %ld frame(s), %ld two-handed grab(s); riding, the "
                      "hands over their drawn grips: the lower hand p50 %.3f / p99 %.3f / max %.3f m, the pair's mean p99 "
                      "%.3f / max %.3f m (%ld frame(s)); the start (both over %.2f m, held %d ms) met on %ld frame(s), "
                      "wheelies from the bars %ld, the lift in the wheelie up to %.3f m, the pull up to %.2f; the previous "
                      "gesture (4 cm from the grab, seat space) would have pulled on %ld of %ld frame(s), up to %.2f "
                      "(its rise up to %.3f m)\n",
                      g.frames, g.grabs, double(g.restLow.Percentile(0.5)), double(g.restLow.Percentile(0.99)),
                      double(g.restLow.max), double(g.restMean.Percentile(0.99)), double(g.restMean.max), g.restLow.count, double(ws.LiftStart()), ws.holdMs,
                      g.wantFrames, g.latches, double(g.maxRoomLift), double(g.maxKeep), g.oldPulls, g.oldFrames,
                      double(g.oldMax), double(g.oldMaxRaise));
        s += w;
    }
    if (v.frames > 0) {
        std::snprintf(b, sizeof(b),
                      "vr wheelie view: %ld VR head-view frame(s), %ld in a wheelie - the bike drawn pitched up to "
                      "%.1f deg, the view pitched up by it at most %.1f deg (%.0f %% of the pitch kept)\n",
                      v.frames, v.pitched, v.maxDrawn, v.maxView,

                      double(rr::game::PlayerWheelie().ViewPitchKeep()) * 100.0);
        s += b;
    }
    return s;
}


} // namespace rrgame
