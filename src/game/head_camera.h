#pragma once
// The first-person camera on the player rider's HEAD - OURS, not the original's: the
// original's views are the four chase cameras of DATA\CAMERA.CA (modes 0..3 of view record +0x21C) and the director's
// scripted ones. This is the fifth entry of rrgame's camera cycle and the base pose a VR headset is composed on.
//
// No GL here: everything is read from the guest arena the frame's PORTED passes left and from the player's own disc.
//
// HOW THE HEAD IS FOUND. The rider is drawn exactly as the renderer draws him (rider_pose_draw.h, `rrgame --posecheck`):
//   * the bike's model matrix is its record's rows +0x1B0 (model axis k = row k, 4096 = 1.0, the lean included) at its
//     box centre +0xB8 (16.16 world units) - one model unit is 1/1024 world unit (race_scene.h kModelUnitsPerWorldUnit);
//   * the rider object hangs at the bike's seat vertex (SeatVertex 0x80066A84 / ChildPlace 0x80066B98: model 100 group 0
//     sub-mesh 0's vertex 3, or the caller's seat for a sidecar rig) plus the root triple owner +0x1C, in the bike's axes;
//   * the rider's 17 part slots (owner +4, the PORTED Pose's output, slot 0 relative to the bike) are walked through the
//     17-part attachment program exactly as rr::PoseGroup does.
// The head is PART 4 of the rider model (BBLEVEL1.GEO model 150 group 0, program 0: root 0, spine 1-2-3, head 4, arms
// 5-7 / 8-10, legs 11-13 / 14-16; rmd3.md 9.3): 51 vertices, 49 quads, the top of the spine chain; its part-local
// extent is x -16..289 (along the spine, the crown at +x), y -121..107 (-y is the face: the feet of parts 13 / 16
// point to -y), z -79..80. Proven per frame against the PORTED model draw's own vertices of the rider (Stats()).
//
// THE CAMERA. Position = the eye point of the posed head part (below). Orientation = the BIKE's frame (+0x1B0: forward
// = model +z, up = model -y, right = forward x up), not the head's animation wobble, so riding feels like sitting on
// the bike; both lightly smoothed (`smoothing`), the position in the bike's frame so it never lags the bike's speed.
// While the rider is OFF the bike (rider +0x25C >= 2: thrown, walking, the crash) `seated` is false and the product
// falls back to the original's chase camera (it follows the rider's box then); the smoothing restarts on re-seating.
//
// AXES AND SCALE (for a VR composition). World units, the PlayStation's axes: Y points DOWN (on a level road `up` has
// y < 0), the handedness the GTE's (x right, y down, z forward in camera space). Model units are millimetres: the
// rider model is 1.68 m tall (1680 units), the bike 2166 units nose to tail (a real ~2.1 m machine), its wheelbase
// 1555 - so kWorldUnitsPerMetre = 1000 / 1024 = 0.977 (one world unit = 1.024 m). Scale a tracked headset offset in
// metres by kWorldUnitsPerMetre and express it in the (right, up, fwd) basis this returns.
#include <cstdint>
#include <vector>

#include "rrformats/pose.h"
#include "rrformats/rmd3.h"
#include "rrformats/skeleton.h"
#include "rrvfs/disc_image.h"

namespace rr::game {

constexpr float kWorldUnitsPerMetre = 1000.0f / 1024.0f;
constexpr float kModelUnitsPerWorldUnitHead = 1024.0f; // = render::kModelUnitsPerWorldUnit (no render dependency here)
constexpr int kHeadPart = 4;                           // the rider model's head sub-mesh (above)

struct HeadPose {
    bool valid = false;
    bool seated = false; // rider +0x25C < 2: on the bike; the camera is meant to be used only then
    // The camera: eye in world units, unit forward / up / right (right = fwd x up in these Y-down axes).
    float eye[3] = {}, fwd[3] = {}, up[3] = {}, right[3] = {};
    // Unsmoothed: the eye point and the head part's centre (the mean of its posed vertices), world units.
    float rawEye[3] = {}, head[3] = {};
    float root[3] = {}; // the rider's root part (the pelvis, part 0) centre, world units (the oracle check's reference)
    // The bike's basis this frame (unsmoothed): its model origin +0xB8 and axes.
    float bikeOrigin[3] = {}, bikeFwd[3] = {}, bikeUp[3] = {}, bikeRight[3] = {};
    // The eye height above the bike's origin along bikeUp, world units (the origin is the box centre, ~0.5 above the road).
    float eyeHeight = 0.0f;
};

class HeadCameraRig {
public:
    // Reads DATA\BBLEVEL1.GEO (models 100 and 150, group 0) and the attachment programs of RASHCDG.BIN. False when the
    // disc lacks them or the rider model is not the 17-part one.
    bool Load(const rr::DiscImage& disc);
    bool Loaded() const { return loaded_; }

    // Once per frame after the frame's step: player `player`'s head camera from bike `bike` (pool 0) and its rider
    // `rider` (the pool-1 owner, bike +0x354). `seat`: the seat vertex in bike model units (null: model 100's LOD 0
    // seat; the product passes the sidecar rig's seat for a two-seat machine). Returns the pose's `valid`.
    bool Update(const uint8_t* ram, uint32_t bike, uint32_t rider, int player, const float* seat = nullptr);
    // The last Update's pose of `player`; false when there is none or it is not valid.
    bool Get(int player, HeadPose& out) const;
    // The unsmoothed pose, no state touched.
    bool Compute(const uint8_t* ram, uint32_t bike, uint32_t rider, const float* seat, HeadPose& out) const;
    // Every LOD-0 vertex of the rider posed by its part slots, placed in the frame `f` (rider_pose.h
    // RiderDrawFrame: model axis c = f.axis[c], the model origin at f.origin), world units, 3 floats a model vertex (the
    // PORTED model draw's capture layout). False when there is no pose.
    bool PoseWorld(const uint8_t* ram, uint32_t rider, const float axis[3][3], const float origin[3],
                   std::vector<float>& world) const;
    void Reset();

    // The rider's LOD-0 sub-meshes the head view does not draw (a bit per sub-mesh): the head (with the helmet).
    // RRJB_HEADCAM_HIDE=<hex mask> (DEVELOPMENT, a rendering check): that mask instead - 0 draws the
    // head too (the view is then inside the helmet).
    uint32_t HiddenRiderParts() const;
    // The VR head view: the rider's own body is not drawn either - a headset's
    // field (~100 degrees) looks down onto the torso, the thighs and the seat from inside them. Hidden: the pelvis 0, the
    // spine 1-3, the head 4, the upper arms 5 and 8, the legs 11-16; kept: the forearms 6 / 9 and the gloves 7 / 10 on
    // the grips (rendered one part at a time from in front of the face - RRJB_HEADCAM_PROBE=1.6 with
    // RRJB_HEADCAM_HIDE = every bit but one: 5 6 7 one shoulder / forearm / glove, 8 9 10 the other's). The
    // RRJB_HEADCAM_HIDE override applies here too.
    uint32_t HiddenRiderPartsVr() const;

    // The oracle check: the head part's centre from the PORTED model draw's own vertices of the rider
    // (shadow_product.h CapturedObject: world = eye + rel per model vertex, model 150 at LOD 0) against Update's posed
    // head of the same frame. Ignored unless the capture is model 150 at LOD 0 with every vertex.
    void NoteCaptured(int player, uint32_t model, int lod, const double eye[3], const std::vector<float>& rel);
    struct Stats {
        size_t updates = 0, seated = 0, invalid = 0, compared = 0, drawn = 0; // drawn: frames the product drew from it
        double sumDist = 0.0, maxDist = 0.0; // world units, posed head centre against the captured one
        // ... against the PREVIOUS frame's posed head (the capture's placement runs a frame behind +0xB8)
        size_t comparedPrev = 0;
        double sumPrev = 0.0, maxPrev = 0.0;
    };
    const Stats& Totals() const { return stats_; }
    void NoteDrawn() { ++stats_.drawn; }

    // The eye in the head part's local frame, model units (Load derives it from the part's extent: 40 % down from the
    // crown, three quarters of the way from the centre line to the face).
    const float* EyeLocal() const { return eyeLocal_; }
    // Per-frame blend toward the new pose (1 = none). 0.5: a half-life of one frame - light.
    float smoothing = 0.5f;
    // The flat-screen view looks this far down from the bike's forward axis (about `right`), so the handlebars and the
    // dash come into the original's 62.6-degree vertical field. A VR layer sets 0: the headset supplies the pitch.
    float lookDownDegrees = 12.0f;
    // How much of the bike's roll (lean) the camera takes: 1 = all of it (the default: the bike's frame), 0 = the
    // horizon kept level (up = the world's up made orthogonal to fwd). A VR layer may want less than 1 for comfort.
    float leanFactor = 1.0f;

private:
    bool loaded_ = false;
    rr::ModelGroup rider_;
    rr::SkeletonTable skeleton_;
    rr::Assembly assembly_;
    float seat_[3] = {};
    float eyeLocal_[3] = {};
    struct Player {
        HeadPose pose;
        bool have = false;
        float offset[3] = {}; // the smoothed eye in the bike's frame (right, up, fwd), world units
        float prevHead[3] = {};
        float fwd[3] = {}, up[3] = {}; // the smoothed bike frame, before the look-down tilt
        bool havePrev = false;
    };
    Player players_[2];
    Stats stats_;
};

// The product's rig (rrgame loads it; a VR layer reads it).
HeadCameraRig& ProductHeadCamera();

// The requested convenience: player `player`'s head camera of the last frame - eye (world units), unit forward and up
// (PlayStation axes, Y down). False when there is none or the rider is off the bike (use the chase view then).
bool HeadCamera(int player, float eye[3], float fwd[3], float up[3]);

} // namespace rr::game
