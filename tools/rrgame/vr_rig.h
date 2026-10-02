#pragma once
// Where the player's eyes are in the race's world. Model: src/platform/xr/vr_rig.h of the gt2-play project (MIT) - the
// same composition, for this game's world:
//
//   world_from_eye[v] = anchor(C . H . S) . recentred(local_from_eye[v])
//
//   C  the game's camera of this frame, in world units and the PlayStation's axes (Y DOWN): the rider's head
//      (game/head_camera.h HeadPose: the bike's frame, the lean included) or the original's chase camera
//      (race_render.h GameCamera: the render camera record).
//   H  the horizon lock: C's pitch and roll taken out by a fraction (1 = level: the bike's lean and the road's slope
//      never tilt the player's horizon; 0 = the camera's frame as it is), its heading kept - a slerp between C's
//      rotation and the level one.
//   S  the seat: metres along the levelled up / forward axes (the seated eye height adjustment).
//   local_from_eye  the runtime's eye pose, made relative to the head's position and heading at the last recentre
//      (race start, the VR menu, the runtime's own recentre event), so the head at recentre time is C's eye.
// One world unit is one metre (g = 0x9D087 / 65536 = 9.8135 units / s^2; the head camera's 1024 model
// units per world unit put a 2166-unit bike at 2.12 m - the 0.977 "mm" reading of head_camera.h differs by 2.4 %,
// below what a seated player notices; `unitsPerMetre` keeps it adjustable).
#include "platform/xr/xr_math.h"
#include "render/mat4.h"

namespace rrgame::vr {

// The camera the rig is anchored on: eye (world units), unit forward and up, the game's Y-down axes.
struct Camera {
    float eye[3] = {0, 0, 0};
    float fwd[3] = {0, 0, 1};
    float up[3] = {0, -1, 0};
    // the share of this camera's roll the view keeps against the horizon lock (0 = the lock alone):
    // the view rolls (1 - lock x (1 - rollKeep)) x the camera's roll - the Modern handling's VR view roll
    float rollKeep = 0.0f;
};

struct RigSettings {
    float horizonLock = 1.0f;  // 0..1
    bool levelPitch = true;    // the lock levels the pitch too (false: the roll only, the product's default)
    float seatUp = 0.0f;       // metres (the levelled up axis)
    float seatForward = 0.0f;  // metres (the levelled forward axis)
    float unitsPerMetre = 1.0f;
    bool lookBack = false;     // the anchor turned 180 degrees about its up axis (look behind)
};

// The runtime's poses made relative to the head at the last recentre (position and heading; the pitch and roll of
// the head stay the head's own).
class Recenter {
public:
    void Request() { pending_ = true; }
    bool Pending() const { return pending_; }
    void Latch(const rr::xr::Pose& head);
    rr::xr::Pose Apply(const rr::xr::Pose& local) const;
    float Yaw() const { return yaw_; }

private:
    bool pending_ = true;
    float offset_[3] = {0, 0, 0};
    float yaw_ = 0;
};

// C . H . S as an OpenXR world anchor (xr_math.h): origin, right / up / ahead in world units.
rr::xr::WorldAnchor LevelledAnchor(const Camera& camera, const RigSettings& settings);

// One eye in the world, ready for race_render.h GameView: the placed eye (position, axes, view matrix) and the
// asymmetric projection of its field of view.
struct Eye {
    rr::xr::WorldEye world;
    rr::render::Mat4 proj;
    rr::xr::Fov fov;
};
Eye PlaceEye(const rr::xr::EyeView& recentredEye, const rr::xr::WorldAnchor& anchor, float nearZ, float farZ);

// One frustum that holds both eyes' (the cull done once for the pair - the union frustum): its apex behind the mid
// eye far enough that both eyes' side planes lie inside, the widest of the four tangents. World units.
rr::render::Mat4 UnionViewProj(const Eye eyes[2], float nearZ, float farZ);

// The yaw rate of the anchor between two frames (radians / second), for the comfort vignette.
float AnchorTurnRate(const rr::xr::WorldAnchor& before, const rr::xr::WorldAnchor& now, double seconds);

} // namespace rrgame::vr
