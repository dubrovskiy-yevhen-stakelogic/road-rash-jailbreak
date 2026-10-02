#pragma once
// When a Touch controller's haptics are written. Portable, no OpenXR: xr_actions.cpp drives the real
// xrApplyHapticFeedback / xrStopHapticFeedback with it and the desktop VR mock (game_host_vr.cpp) counts what it
// would have sent.
//
// The game's rumble is a LEVEL, as the console's is: libpad sends the DualShock the two motor bytes every frame and
// the motor keeps turning at the last value (rumble_product.h). A Touch actuator is not a motor: every
// xrApplyHapticFeedback restarts a vibration of a given length, so re-sending the same level every frame (40 ms
// pulses at 60..120 calls a second per hand, twice a frame with the hands' own haptics merged in) restarts the
// actuator 60..240 times a second - a buzz of its own that is heard and felt.
//
// The policy, per hand, on the level the host asks for this frame (0..1, the vibration setting already applied):
//   * 0 (below kFloor): one xrStopHapticFeedback when something is playing, then nothing;
//   * a level while nothing plays: one pulse of kPulseNs at that level;
//   * a level while a pulse plays: a new pulse only when the level moved by kStep or more (and not within kMinGapNs
//     of the last one - a level that changes every frame, the grips' road feel, is followed at most 50 times a
//     second), or when the pulse has less than kRenewNs left (the renewal: one call per ~200 ms while it holds). A host
//     that stops calling (the VR menu holding the race) leaves at most one pulse of kPulseNs running out.
// RRJB_HAPTICS=frame restores the old per-frame behaviour (the negative control of the measurement).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace rr::xr {

struct HapticCommand {
    enum Kind { kNone, kApply, kStop } kind = kNone;
    float amplitude = 0.0f;
    int64_t durationNs = 0;
};

class HapticPulse {
public:
    static constexpr float kFloor = 0.01f;
    static constexpr float kStep = 0.04f; // about 10 of the large motor's 255
    static constexpr int64_t kPulseNs = 250'000'000;
    static constexpr int64_t kRenewNs = 50'000'000;
    static constexpr int64_t kMinGapNs = 20'000'000;
    static constexpr int64_t kLegacyPulseNs = 40'000'000; // the pulse length of the per-frame mode (RRJB_HAPTICS=frame)

    // RRJB_HAPTICS=frame: the per-frame behaviour (a pulse on every call with a level, a stop on the fall to 0).
    static bool LegacyRequested() {
        const char* e = std::getenv("RRJB_HAPTICS");
        return e != nullptr && std::strcmp(e, "frame") == 0;
    }
    explicit HapticPulse(bool legacy = LegacyRequested()) : legacy_(legacy) {}

    // This frame's level for the hand at `nowNs` (a monotonic clock); what to send.
    HapticCommand Update(float level, int64_t nowNs) {
        const float a = std::clamp(level, 0.0f, 1.0f);
        HapticCommand c;
        if (a < kFloor) {
            if (playing_) {
                c.kind = HapticCommand::kStop;
                playing_ = false;
                ++stops_;
            }
            return c;
        }
        bool send = true;
        if (!legacy_ && playing_ && nowNs < endsNs_) {
            const bool moved = std::fabs(a - applied_) >= kStep && nowNs - appliedAtNs_ >= kMinGapNs;
            const bool renew = endsNs_ - nowNs <= kRenewNs;
            send = moved || renew;
        }
        if (!send) return c;
        c.kind = HapticCommand::kApply;
        c.amplitude = a;
        c.durationNs = legacy_ ? kLegacyPulseNs : kPulseNs;
        playing_ = true;
        applied_ = a;
        appliedAtNs_ = nowNs;
        endsNs_ = nowNs + c.durationNs;
        ++applies_;
        return c;
    }
    bool Playing() const { return playing_; }
    bool Legacy() const { return legacy_; }
    uint64_t Applies() const { return applies_; }
    uint64_t Stops() const { return stops_; }

private:
    bool legacy_ = false, playing_ = false;
    float applied_ = 0.0f;
    int64_t appliedAtNs_ = 0, endsNs_ = 0;
    uint64_t applies_ = 0, stops_ = 0;
};

} // namespace rr::xr
