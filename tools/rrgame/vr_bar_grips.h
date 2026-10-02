#pragma once
// Where the handlebar grips are: the ends of the BIKE MODEL's own handlebar, posed as the renderer poses
// the bike (BBLEVEL1.GEO model 100 group 0, the attachment program of RASHCDG.BIN, the part slots BikeInstance
// 0x80084E10 leaves - bike_pose_product.h ReadBikeParts), in the bike's frame (+0x1B0 at +0xB8: the lean and the
// heading included).
//
// The handlebar: model 100's sub-mesh 1 - the fork part, the one of the four moving parts that reaches widest and
// highest (the probe, RRJB_BARS_PROBE=1: 45 vertices, x -0.365..+0.359, up 0.21..1.01 world units in its own frame;
// the wheels are sub-meshes 2 / 3, the body 4). Its bar ends are the vertices within 3 cm of its lateral extreme on
// each side (four a side on the USA disc); a grip - where the fist's centre sits - is half a hand's width (4.5 cm)
// inboard of its bar end.
//   * The REST grips (the fork part at rest) are what the steering is measured against: they never turn with the
//     game's own fork animation, so the bars the player turns cannot feed back into their own reference.
//   * The POSED grips (this frame's fork slot) are where the grips are drawn to reach for.
// The rider's gloves (rider model 150 parts 7 / 10, posed like head_camera.h) are logged once against the bars: the
// original's rider holds his hands about 15 cm outside the bar ends (his gloves at +-0.44..0.57, the bar ends at
// +-0.36 - the rider mesh is drawn wider than the bike's bar), so the gloves are not used as the grips.
// No GL, no state in the guest.
#include <cstdint>

#include "render/mat4.h"
#include "rrformats/pose.h"
#include "rrformats/rmd3.h"
#include "rrformats/skeleton.h"
#include "rrvfs/disc_image.h"
#include "vr_visual_lean.h"

namespace rrgame {

struct BikeFrame {
    bool valid = false;
    bool seated = false;          // rider +0x25C < 2
    float origin[3] = {};         // +0xB8, world units (PlayStation axes, Y down)
    float right[3] = {}, up[3] = {}, fwd[3] = {}; // the bike's axes (up = -model y, right = fwd x up)
    float speed = 0.0f;           // +0x1E0, world units / s
    float headingDegrees = 0.0f;  // the forward axis' heading in the x/z plane (log only)
};

class BarGrips {
public:
    static constexpr int kLeftGlove = 7, kRightGlove = 10; // the rider's glove parts (the log's comparison)
    static constexpr int kForkPart = 1;                    // model 100's handlebar part (checked at Load)
    bool Load(const rr::DiscImage& disc);
    bool Loaded() const { return loaded_; }
    // Once a frame after the frame's step. `seat`: the rider's seat vertex in bike model units (null: model 100's).
    // `leanScale` (vr_visual_lean.h): the share of the original's roll the bike is drawn with - the frame,
    // the model matrix and so the grips are those of the DRAWN bike (1: the record's own).
    // `drawnRoll`: the Modern handling's lean to draw instead of the original's roll (`leanScale` of it)
    bool Update(const uint8_t* ram, uint32_t bike, uint32_t rider, const float* seat, float leanScale = 1.0f,
                const float* drawnRoll = nullptr);
    // The visual lean Update applied this frame (off: none), and the wheels' contact point below the origin along the
    // bike's up (world units, <= 0: the rest model's lowest vertex).
    const VisualLean& Lean() const { return lean_; }
    float ContactUp() const { return contactUp_; }
    // Each wheel's contact at rest (vr_horizon.h), in the bike's frame (world units): [0] up, [1] fwd
    bool WheelContacts(float front[2], float rear[2]) const {
        if (!haveWheels_) return false;
        front[0] = wheelFront_[0], front[1] = wheelFront_[1], rear[0] = wheelRear_[0], rear[1] = wheelRear_[1];
        return true;
    }
    void Reset();
    const BikeFrame& Frame() const { return frame_; }
    // The grips in the bike's frame (right, up, fwd), world units; [0] left, [1] right: at rest (the steering's
    // reference) and as the game posed the fork this frame (drawn). False before Load.
    bool Grips(float out[2][3]) const;
    bool PosedGrips(float out[2][3]) const;
    // The rider's gloves in the bike's frame (the log), [0] left, [1] right; false until measured on a seated frame.
    bool Gloves(float out[2][3]) const;
    // The rider part of the LEFT glove (7 or 10, by the side it was measured on; kLeftGlove until measured).
    int LeftGlovePart() const { return leftGlovePart_; }
    // A point of the bike's frame (right, up, fwd, world units) in the world, and back.
    void ToWorld(const float local[3], float world[3]) const;
    void ToBike(const float world[3], float local[3]) const;
    // ---- the held hands ride the grips of the bike AS DRAWN.
    // The grips in the bike MODEL's own space (model units: x right, y down, z forward - what the drawn model matrix
    // takes), the fork posed by `parts` (the five part slots; null: at rest): [0] left, [1] right - each side's bar-end
    // mean half a hand inboard - and the fork's turn from its rest pose in the model's axes (row-major, posed * rest^T:
    // the steering and whatever its parents - the body's pitch slot - add; the identity at rest).
    bool ModelGrips(const rr::PartMatrix* parts, float pos[2][3], float rot[9]) const;
    // This frame's model matrix of the bike exactly as the renderer builds it (race_render.cpp RecordModelMatrix:
    // +0x1B0's rows / 4096 as the model axes, 1 / 1024 world units per model unit, the origin +0xB8) and the part
    // slots Update read (false: the rest pose). Valid after an Update that returned true.
    const rr::render::Mat4& Model() const { return model_; }
    const rr::PartMatrix* Parts() const { return haveParts_ ? parts_ : nullptr; }
    // the fork DRAWN turned `radians` (positive: the bars turned left, the right end forward) about its
    // own steering axis - the part-local y of its slot, which BikeInstance fills with RotMatrix(0, +0x33A steer, 0) -
    // in place of the game's own slot (on = false: the game's slot back). Re-poses the grips; the simulation's slot
    // in the guest is not touched. Call after Update (which reads the game's slot afresh).
    void SetForkTurn(bool on, float radians);
    bool ForkTurned() const { return forkTurned_; }
    const rr::PartMatrix& Fork() const { return parts_[kForkPart]; }

private:
    bool BarEnds(const rr::PosedGroup& posed, float out[2][3]) const;
    bool loaded_ = false;
    rr::ModelGroup bike_;
    rr::Assembly bikeAssembly_;
    rr::ModelGroup rider_;
    rr::Assembly riderAssembly_;
    bool haveRider_ = false;
    rr::SkeletonTable skeleton_;
    float seat_[3] = {};
    std::vector<uint32_t> ends_[2]; // the fork part's bar-end vertex indices, left / right
    BikeFrame frame_;
    float rest_[2][3] = {}, posed_[2][3] = {};
    rr::render::Mat4 model_;
    rr::PartMatrix parts_[5];
    bool haveParts_ = false;
    rr::PartMatrix gameFork_;       // the game's own fork slot this frame (SetForkTurn's off)
    bool forkTurned_ = false;
    VisualLean lean_;
    float contactUp_ = 0.0f;
    bool haveWheels_ = false;
    float wheelFront_[2] = {}, wheelRear_[2] = {};   // up, fwd
    rr::PartMatrix restFork_;      // the fork part's rest rotation (part-local -> model)
    bool haveGloves_ = false;
    int leftGlovePart_ = kLeftGlove;
    float glove_[2][3] = {};
};

} // namespace rrgame
