#pragma once
// The player's own bike in the head view - OURS, no original code: what shakes on the bike under the
// rider's eye, measured and, render-only, calmed.
//
// The bike model (BBLEVEL1.GEO model 100) is five sub-meshes: 0 the carrier (4 vertices), 1 the fork (the handlebars,
// the headlight, the dash), 2 / 3 the wheels, 4 the body (the tank, the seat, the fairing). The attachment program places
// sub-meshes 1, 2, 3, 4 with the part SLOTS 1, 3, 4, 2 (skeleton.h AttachLink), which BikeInstance RASHCDG 0x80084E10 and
// WheelSlots 0x80066EC4 (both PORTED, bike_pose_product.h) fill once per race step:
//   slot 1 = RotMatrix(0, +0x33A, 0)        the fork's steering
//   slot 2 = RotMatrix(+0x34E, 0, roll)     the body's pitch spring (+0x34C its velocity; roll: a passenger's lean)
//   slot 3 / 4 = RotMatrix(+0x344 / +0x346)  the wheels' spin
// The head view's eye is fixed on the bike's ROOT (vr_handlebars.h SeatEye), so whatever slot 2 turns - the body
// and whatever hangs below it in the chain - moves against the eye whenever the spring moves.
//
//   * BikeShakeMeter: per displayed head-view frame, the angular motion of the drawn bike's vertices in the eye (the
//     direction from the eye to each vertex, in the camera's axes), per group (the handlebars' ends, the fork, the body,
//     the wheels), decomposed into the root pose (every slot at rest: the stabilised bike and the camera), each slot's
//     own displacement (slot k alone against the rest pose) and the eye's anchor in the drawn bike's frame. Reported as
//     the RMS of the first difference (deg / frame: the motion) and of the second difference (deg / frame^2: the
//     shake), with the dominant frequency of the high-passed signals. RRJB_SHAKE_LOG=<csv>: one row a frame.
//   * THE MEASURED CAUSE: not the slots but the bike's own orientation rows +0x1B0 - the game rebuilds them
//     every step from rows whose length creeps from 0.998 to 1.006 over ~25 ticks and then snaps back, a 12 Hz sawtooth
//     in the SCALE of the drawn bike. Seen from the chase camera it is nothing; with the eye fixed on the bike's
//     (normalised) frame 0.8 m above the bars it moves the handlebars ~5 mm (0.4 deg) every 1/12 s.
//   * BikeShakeFilter: the player's bike in the head view (VR and the desktop head camera) - its rows +0x1B0 at unit
//     length (the direction the game gave: forward kept, up orthogonalised, right from both) and its part slots,
//     written into the arena for the frame's drawing and put back byte for byte before the next step (like
//     FrameInterp, vr_comfort.h): every consumer - the renderer, the rider on the bike, the grips the hands sit on
//     (BarGrips::Update reads the rows and the slots), the weapon on the bars - sees the same drawn bike.
//     [vr] bike_shake (--vr-bike-shake off | low | original):
//       original  the game's rows and slots as the step left them (the unfiltered picture);
//       low       unit rows; the slots between the last two steps (smooth motion's alpha), the pitch spring (and a
//                 passenger's roll) through a critically damped low-pass (kLowHz) at kLowGain of its amplitude: the
//                 braking dive and the throttle's squat stay as a slow, gentle tilt; the fork (the steering) and the
//                 wheels only interpolated;
//       off       unit rows; the pitch slot (and the roll) held at rest; the fork and the wheels interpolated.
//     The desktop head camera (--camera head) takes the same value; the console camera and the chase views are never
//     touched (the player's bike seen from outside keeps the game's slots).
#include "render/mat4.h"
#include "rrformats/pose.h"
#include "rrformats/rmd3.h"
#include "rrformats/skeleton.h"
#include "rrvfs/disc_image.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace rrgame {

enum BikeShakeMode { kShakeOff = 0, kShakeLow = 1, kShakeOriginal = 2 };
const char* BikeShakeName(int mode);                  // "Off" / "Low" / "Original"
bool ParseBikeShake(const std::string& v, int& mode); // off | low | original

// ---------------------------------------------------------------- the filter
class BikeShakeFilter {
public:
    static constexpr double kLowHz = 1.2;   // the low-pass' natural frequency (Hz)
    static constexpr double kLowGain = 0.5; // the drawn share of the filtered pitch
    // After each step: the player's bike's slots of the new state (the previous capture becomes the step before).
    void Capture(const uint8_t* ram, uint32_t bike);
    void Invalidate() {
        have_ = 0;
        state_ = State{};
    }
    // One displayed frame of the head view: the drawn slots written into the arena (`alpha`: the drawn time between the
    // last two steps, 1 = the last step; `dt`: the display period, s; after Break the low-pass starts again from the
    // game's slot). False when nothing was written (original, not a five-part bike, no capture). Every true Apply is
    // followed by Restore before the next step.
    bool Apply(uint8_t* ram, uint32_t bike, int mode, float alpha, double dt);
    void Restore(uint8_t* ram);
    bool Applied() const { return !saved_.empty(); }
    void Break() { state_.have = false; } // a frame that is not the head view: the next Apply starts again
    // The last step's own slots (0 the identity, 1..4 as the game left them); false without a capture.
    bool GameParts(uint32_t bike, rr::PartMatrix out[5]) const;
    std::string Totals() const; // one report line

private:
    struct Slots {
        uint32_t bike = 0, parts = 0;
        double fork = 0, pitch = 0, roll = 0, wheel[2] = {}; // radians
        int16_t raw[5][9] = {};
        bool valid = false;
    };
    Slots prev_, cur_;
    int have_ = 0;
    struct State {
        bool have = false;
        uint32_t bike = 0;
        double x[2] = {}, v[2] = {}; // the pitch and the roll through the low-pass, and their rates
    } state_;
    struct Saved {
        uint32_t addr = 0;
        uint8_t bytes[18] = {};
    };
    std::vector<Saved> saved_;
    // the run's counters
    size_t frames_ = 0, restores_ = 0, restoreMismatch_ = 0, skipped_ = 0, pitchN_ = 0;
    double pitchGameSq_ = 0, pitchDrawnSq_ = 0, pitchGameMax_ = 0, pitchDrawnMax_ = 0; // degrees
    double rowLenMin_ = 1e9, rowLenMax_ = 0;                                            // the rows +0x1B0 (1.0 = 4096)
};
BikeShakeFilter& ProductBikeShake();

// ---------------------------------------------------------------- the meter
class BikeShakeMeter {
public:
    bool Load(const rr::DiscImage& disc);
    bool Loaded() const { return loaded_; }
    // One displayed head-view frame: the camera (eye, forward, up; world), the drawn bike's model matrix and part slots
    // (race_render.h DrawnMachine; null parts: the rest pose), `unitsPerMetre` (the mm of the eye's anchor), `period`
    // (s, the frequencies), `gameParts`: the game's own slots of this step (the CSV's reference; may be null).
    void Frame(long frame, const float eye[3], const float fwd[3], const float up[3], float unitsPerMetre, double period,
               const rr::render::Mat4& model, const rr::PartMatrix* parts, const rr::PartMatrix* gameParts);
    void Break() { n_ = 0; }
    std::string Totals() const;

private:
    static constexpr int kConfigs = 6; // 0 drawn (every slot), 1 the rest pose (the root), 2..5 slot 1..4 alone
    enum Group { kBarEnds = 0, kFork, kBody, kWheels, kGroups };
    bool loaded_ = false;
    rr::ModelGroup bike_;
    rr::Assembly assembly_;
    rr::SkeletonTable skeleton_;
    std::vector<uint32_t> probe_;      // every probed vertex (index into bike_.verts)
    std::vector<uint8_t> probePart_;   // its sub-mesh
    std::vector<uint8_t> probeGroups_; // its groups (bit mask)
    size_t groupCount_[kGroups] = {};
    struct Sample {
        std::vector<float> dir[kConfigs]; // per config, 3 floats per probed vertex: the unit direction in the camera
        float eyeLocal[3] = {};           // the eye in the drawn bike's root frame (world units)
        float camLocal[9] = {};           // the camera's right / up / forward in the bike's frame
    };
    Sample s_[3];
    int n_ = 0;
    size_t frames_ = 0, n2_ = 0;
    double vel_[kConfigs][kGroups] = {}, acc_[kConfigs][kGroups] = {}; // sums of squares (deg)
    double slotVel_[4][kGroups] = {}, slotAcc_[4][kGroups] = {};      // slot k's own displacement
    double accMax_[kGroups] = {};
    double eyePosAcc_ = 0, eyeAngAcc_ = 0, eyePosMax_ = 0; // mm, deg
    float mmPerUnit_ = 1000.0f;
    double nearest_ = 1e9; // the drawn bike's nearest vertex to the eye over the run (mm)
    // the series for the frequencies (the bar ends' mean elevation in the eye, deg): drawn, root, slot 1..4's own
    std::vector<float> series_[6];
    std::vector<float> periods_;
    std::FILE* csv_ = nullptr;
    bool csvTried_ = false;
};
BikeShakeMeter& ProductBikeShakeMeter();

} // namespace rrgame
