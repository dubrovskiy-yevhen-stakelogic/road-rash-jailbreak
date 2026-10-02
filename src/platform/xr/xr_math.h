#pragma once
// The small amount of geometry the OpenXR layer needs, in the renderer's own matrix type (render/mat4.h: column-major,
// the layout glUniformMatrix4fv takes with transpose = GL_FALSE, GL's clip space with z in [-w, w]).
//
// Header-only and free of OpenXR types, so the game side can use it without the OpenXR headers:
//   * Pose / Fov / EyeView are what xrLocateViews reports (metres, the runtime's LOCAL or STAGE space, y up, -z ahead);
//   * ProjectionFromFov is the asymmetric frustum of one eye (the four tangents OpenXR gives, not a symmetric fov);
//   * WorldAnchor ties the XR space to the game's world: which world point the space's origin is, which world
//     directions its x (right), y (up) and -z (ahead) are, and how many world units one metre is. The game's world is
//     y-down (a road's normal has a negative y towards the sky, rrview/main.cpp); the anchor's `up` is whatever the
//     camera's up is, so no axis convention is baked in here.
#include "render/mat4.h"

#include <cmath>

namespace rr::xr {

struct Pose {
    float position[3] = {0, 0, 0};
    float orientation[4] = {0, 0, 0, 1}; // quaternion x, y, z, w
};

struct Fov {
    float left = -0.8f, right = 0.8f, up = 0.8f, down = -0.8f; // radians, as XrFovf
};

struct EyeView {
    Pose pose;
    Fov fov;
};

// The rotation of a unit quaternion as a 3x3, column-major (c[0..2] the image of x, c[3..5] of y, c[6..8] of z).
inline void QuatToMatrix3(const float q[4], float c[9]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    c[0] = 1 - 2 * (y * y + z * z); c[1] = 2 * (x * y + z * w);     c[2] = 2 * (x * z - y * w);
    c[3] = 2 * (x * y - z * w);     c[4] = 1 - 2 * (x * x + z * z); c[5] = 2 * (y * z + x * w);
    c[6] = 2 * (x * z + y * w);     c[7] = 2 * (y * z - x * w);     c[8] = 1 - 2 * (x * x + y * y);
}

// The OpenXR projection of one eye for GL: near / far in the units the view matrix produces.
inline render::Mat4 ProjectionFromFov(const Fov& fov, float nearZ, float farZ) {
    const float l = std::tan(fov.left), r = std::tan(fov.right), u = std::tan(fov.up), d = std::tan(fov.down);
    render::Mat4 m;
    for (float& v : m.m) v = 0.0f;
    m.m[0] = 2.0f / (r - l);
    m.m[5] = 2.0f / (u - d);
    m.m[8] = (r + l) / (r - l);
    m.m[9] = (u + d) / (u - d);
    m.m[10] = -(farZ + nearZ) / (farZ - nearZ);
    m.m[11] = -1.0f;
    m.m[14] = -(2.0f * farZ * nearZ) / (farZ - nearZ);
    return m;
}

// Where the XR space sits in the game's world.
struct WorldAnchor {
    float origin[3] = {0, 0, 0};
    float right[3] = {1, 0, 0};
    float up[3] = {0, 1, 0};
    float ahead[3] = {0, 0, -1};
    float unitsPerMetre = 1.0f;
};

// An anchor from a camera-style (eye, ahead, up) triple: right = ahead x up (the same right LookAt builds), and up
// re-orthogonalised against ahead. The three form a proper rotation, so the world is never mirrored.
inline WorldAnchor AnchorFrom(const float origin[3], const float aheadIn[3], const float upIn[3], float unitsPerMetre) {
    WorldAnchor a;
    float f[3] = {aheadIn[0], aheadIn[1], aheadIn[2]};
    float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (float& v : f) v /= fl > 0 ? fl : 1.0f;
    float r[3] = {f[1] * upIn[2] - f[2] * upIn[1], f[2] * upIn[0] - f[0] * upIn[2], f[0] * upIn[1] - f[1] * upIn[0]};
    float rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    for (float& v : r) v /= rl > 0 ? rl : 1.0f;
    const float u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
    for (int k = 0; k < 3; ++k) {
        a.origin[k] = origin[k];
        a.ahead[k] = f[k];
        a.right[k] = r[k];
        a.up[k] = u[k];
    }
    a.unitsPerMetre = unitsPerMetre;
    return a;
}

// One eye (or the head) placed in the world: its position, and its three axes as world directions.
struct WorldEye {
    float eye[3];
    float right[3], up[3], forward[3];
    render::Mat4 view; // world -> eye, rigid (world units)
};

inline WorldEye PlaceInWorld(const Pose& local, const WorldAnchor& a) {
    float c[9];
    QuatToMatrix3(local.orientation, c);
    // local axis k of the XR space in world: x -> right, y -> up, z -> -ahead
    auto toWorld = [&](const float v[3], float out[3]) {
        for (int k = 0; k < 3; ++k) out[k] = a.right[k] * v[0] + a.up[k] * v[1] - a.ahead[k] * v[2];
    };
    WorldEye w;
    float p[3];
    toWorld(local.position, p);
    for (int k = 0; k < 3; ++k) w.eye[k] = a.origin[k] + p[k] * a.unitsPerMetre;
    const float lx[3] = {c[0], c[1], c[2]}, ly[3] = {c[3], c[4], c[5]}, lz[3] = {c[6], c[7], c[8]};
    float back[3];
    toWorld(lx, w.right);
    toWorld(ly, w.up);
    toWorld(lz, back);
    for (int k = 0; k < 3; ++k) w.forward[k] = -back[k];
    render::Mat4& v = w.view;
    for (int k = 0; k < 3; ++k) {
        v.m[4 * k + 0] = w.right[k];
        v.m[4 * k + 1] = w.up[k];
        v.m[4 * k + 2] = back[k];
        v.m[4 * k + 3] = 0.0f;
    }
    v.m[12] = -(w.right[0] * w.eye[0] + w.right[1] * w.eye[1] + w.right[2] * w.eye[2]);
    v.m[13] = -(w.up[0] * w.eye[0] + w.up[1] * w.eye[1] + w.up[2] * w.eye[2]);
    v.m[14] = -(back[0] * w.eye[0] + back[1] * w.eye[1] + back[2] * w.eye[2]);
    v.m[15] = 1.0f;
    return w;
}

// The yaw-only part of a pose (rotation about the space's y axis): what a theatre screen or a recentred seat keeps.
inline float YawOf(const Pose& p) {
    float c[9];
    QuatToMatrix3(p.orientation, c);
    // forward = -z column, projected on the horizontal plane
    return std::atan2(c[6], c[8]); // angle of the back axis: yaw 0 = looking down -z
}

} // namespace rr::xr
