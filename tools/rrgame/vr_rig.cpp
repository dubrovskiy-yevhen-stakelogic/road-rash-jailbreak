// The VR rig of the race (vr_rig.h). The quaternion helpers and the recentre follow vr_rig.cpp of the gt2-play
// project (MIT).
#include "vr_rig.h"

#include <algorithm>
#include <cmath>

namespace rrgame::vr {
namespace {

float Dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void Cross(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

bool Normalise3(float v[3]) {
    const float n = std::sqrt(Dot(v, v));
    if (n < 1e-8f) return false;
    for (int k = 0; k < 3; ++k) v[k] /= n;
    return true;
}

void Normalise4(float q[4]) {
    const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n > 0)
        for (int i = 0; i < 4; i++) q[i] /= n;
}

// Column-major 3x3 (columns right, up, back) <-> unit quaternion (x, y, z, w).
void QuatToMatrix(const float q[4], float m[9]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = 1 - 2 * (y * y + z * z);
    m[1] = 2 * (x * y + z * w);
    m[2] = 2 * (x * z - y * w);
    m[3] = 2 * (x * y - z * w);
    m[4] = 1 - 2 * (x * x + z * z);
    m[5] = 2 * (y * z + x * w);
    m[6] = 2 * (x * z + y * w);
    m[7] = 2 * (y * z - x * w);
    m[8] = 1 - 2 * (x * x + y * y);
}

void MatrixToQuat(const float m[9], float q[4]) {
    const float trace = m[0] + m[4] + m[8];
    if (trace > 0) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q[3] = 0.25f * s;
        q[0] = (m[5] - m[7]) / s;
        q[1] = (m[6] - m[2]) / s;
        q[2] = (m[1] - m[3]) / s;
    } else if (m[0] > m[4] && m[0] > m[8]) {
        const float s = std::sqrt(1.0f + m[0] - m[4] - m[8]) * 2.0f;
        q[3] = (m[5] - m[7]) / s;
        q[0] = 0.25f * s;
        q[1] = (m[3] + m[1]) / s;
        q[2] = (m[6] + m[2]) / s;
    } else if (m[4] > m[8]) {
        const float s = std::sqrt(1.0f + m[4] - m[0] - m[8]) * 2.0f;
        q[3] = (m[6] - m[2]) / s;
        q[0] = (m[3] + m[1]) / s;
        q[1] = 0.25f * s;
        q[2] = (m[7] + m[5]) / s;
    } else {
        const float s = std::sqrt(1.0f + m[8] - m[0] - m[4]) * 2.0f;
        q[3] = (m[1] - m[3]) / s;
        q[0] = (m[6] + m[2]) / s;
        q[1] = (m[7] + m[5]) / s;
        q[2] = 0.25f * s;
    }
    Normalise4(q);
}

void Slerp(const float a[4], const float b0[4], float t, float out[4]) {
    float b[4] = {b0[0], b0[1], b0[2], b0[3]};
    float cosine = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (cosine < 0) {
        cosine = -cosine;
        for (int i = 0; i < 4; i++) b[i] = -b[i];
    }
    if (cosine > 0.9995f) {
        for (int i = 0; i < 4; i++) out[i] = a[i] + (b[i] - a[i]) * t;
        Normalise4(out);
        return;
    }
    const float angle = std::acos(std::clamp(cosine, -1.0f, 1.0f));
    const float s = std::sin(angle);
    const float wa = std::sin((1 - t) * angle) / s, wb = std::sin(t * angle) / s;
    for (int i = 0; i < 4; i++) out[i] = a[i] * wa + b[i] * wb;
    Normalise4(out);
}

// A frame (ahead, up) as the rotation whose columns are right = ahead x up, up, back = -ahead (xr_math.h AnchorFrom's
// right; a proper rotation for any orthonormal pair).
void FrameMatrix(const float ahead[3], const float up[3], float m[9]) {
    float r[3];
    Cross(ahead, up, r);
    for (int k = 0; k < 3; ++k) {
        m[k] = r[k];
        m[3 + k] = up[k];
        m[6 + k] = -ahead[k];
    }
}

} // namespace

void Recenter::Latch(const rr::xr::Pose& head) {
    offset_[0] = head.position[0];
    offset_[1] = head.position[1];
    offset_[2] = head.position[2];
    float m[9];
    QuatToMatrix(head.orientation, m);
    // the head's forward axis (-back = -column 2) projected on the horizontal plane
    const float fx = -m[6], fz = -m[8];
    yaw_ = (std::abs(fx) > 1e-8f || std::abs(fz) > 1e-8f) ? std::atan2(-fx, -fz) : 0.0f;
    pending_ = false;
}

rr::xr::Pose Recenter::Apply(const rr::xr::Pose& local) const {
    const float c = std::cos(-yaw_), s = std::sin(-yaw_);
    const float d[3] = {local.position[0] - offset_[0], local.position[1] - offset_[1], local.position[2] - offset_[2]};
    rr::xr::Pose out;
    out.position[0] = c * d[0] + s * d[2];
    out.position[1] = d[1];
    out.position[2] = -s * d[0] + c * d[2];
    const float q[4] = {0, std::sin(-yaw_ * 0.5f), 0, std::cos(-yaw_ * 0.5f)}; // Ry(-yaw)
    const float* p = local.orientation;
    out.orientation[0] = q[3] * p[0] + q[0] * p[3] + q[1] * p[2] - q[2] * p[1];
    out.orientation[1] = q[3] * p[1] - q[0] * p[2] + q[1] * p[3] + q[2] * p[0];
    out.orientation[2] = q[3] * p[2] + q[0] * p[1] - q[1] * p[0] + q[2] * p[3];
    out.orientation[3] = q[3] * p[3] - q[0] * p[0] - q[1] * p[1] - q[2] * p[2];
    Normalise4(out.orientation);
    return out;
}

rr::xr::WorldAnchor LevelledAnchor(const Camera& camera, const RigSettings& settings) {
    const float worldUp[3] = {0.0f, -1.0f, 0.0f}; // the PlayStation's Y points down
    float f[3] = {camera.fwd[0], camera.fwd[1], camera.fwd[2]};
    if (!Normalise3(f)) f[2] = 1.0f;
    float u[3] = {camera.up[0], camera.up[1], camera.up[2]};
    { // the camera's up made orthogonal to its forward
        const float d = Dot(u, f);
        for (int k = 0; k < 3; ++k) u[k] -= d * f[k];
        if (!Normalise3(u)) for (int k = 0; k < 3; ++k) u[k] = worldUp[k];
    }
    // the level frame: the forward's horizontal part, the world's up
    float h[3] = {f[0], f[1], f[2]};
    {
        const float d = Dot(h, worldUp);
        for (int k = 0; k < 3; ++k) h[k] -= d * worldUp[k];
        if (!Normalise3(h)) { // looking straight up or down: the heading from the camera's up instead
            for (int k = 0; k < 3; ++k) h[k] = -u[k] * (Dot(f, worldUp) > 0 ? -1.0f : 1.0f);
            const float e = Dot(h, worldUp);
            for (int k = 0; k < 3; ++k) h[k] -= e * worldUp[k];
            if (!Normalise3(h)) h[2] = 1.0f;
        }
    }
    float ahead[3], up[3];
    const float lock = std::clamp(settings.horizonLock, 0.0f, 1.0f);
    if (lock <= 0.0f) {
        for (int k = 0; k < 3; ++k) ahead[k] = f[k], up[k] = u[k];
    } else if (!settings.levelPitch) { // the roll only - forward kept, up = the world's up square to it
        float lv[3] = {worldUp[0], worldUp[1], worldUp[2]};
        const float d = Dot(lv, f);
        for (int k = 0; k < 3; ++k) lv[k] -= d * f[k];
        if (!Normalise3(lv)) for (int k = 0; k < 3; ++k) lv[k] = u[k];
        if (lock >= 1.0f) {
            for (int k = 0; k < 3; ++k) ahead[k] = f[k], up[k] = lv[k];
        } else {
            float mc[9], ml[9], qc[4], ql[4], q[4], m[9];
            FrameMatrix(f, u, mc);
            FrameMatrix(f, lv, ml);
            MatrixToQuat(mc, qc);
            MatrixToQuat(ml, ql);
            Slerp(qc, ql, lock, q);
            QuatToMatrix(q, m);
            for (int k = 0; k < 3; ++k) {
                up[k] = m[3 + k];
                ahead[k] = -m[6 + k];
            }
        }
    } else if (lock >= 1.0f) {
        for (int k = 0; k < 3; ++k) ahead[k] = h[k], up[k] = worldUp[k];
    } else {
        float mc[9], ml[9], qc[4], ql[4], q[4], m[9];
        FrameMatrix(f, u, mc);
        FrameMatrix(h, worldUp, ml);
        MatrixToQuat(mc, qc);
        MatrixToQuat(ml, ql);
        Slerp(qc, ql, lock, q);
        QuatToMatrix(q, m);
        for (int k = 0; k < 3; ++k) {
            up[k] = m[3 + k];
            ahead[k] = -m[6 + k];
        }
    }
    const float keep = std::clamp(camera.rollKeep, 0.0f, 1.0f);
    if (keep > 0.0f && lock > 0.0f) { // a share of the camera's roll back into the levelled view
        // the signed roll of a frame (forward, up) against the level plane through its forward (vr_visual_lean.h's)
        const auto rollOf = [&worldUp](const float fw[3], const float upv[3]) {
            float lv[3] = {worldUp[0], worldUp[1], worldUp[2]};
            const float d = Dot(lv, fw);
            for (int k = 0; k < 3; ++k) lv[k] -= d * fw[k];
            if (!Normalise3(lv)) return 0.0f;
            float cr[3];
            Cross(lv, upv, cr);
            return std::atan2(Dot(cr, fw), Dot(lv, upv));
        };
        const float want = (1.0f - lock * (1.0f - keep)) * rollOf(f, u), now = rollOf(ahead, up);
        // turn up about ahead by (want - now) (Rodrigues; up is square to ahead)
        const float a = want - now, c = std::cos(a), s = std::sin(a);
        float x[3];
        Cross(ahead, up, x);
        for (int k = 0; k < 3; ++k) up[k] = up[k] * c + x[k] * s;
        Normalise3(up);
    }
    if (settings.lookBack)
        for (float& v : ahead) v = -v;
    float origin[3];
    for (int k = 0; k < 3; ++k)
        origin[k] = camera.eye[k] + (up[k] * settings.seatUp + ahead[k] * (settings.lookBack ? -1.0f : 1.0f) * settings.seatForward) *
                                        settings.unitsPerMetre;
    return rr::xr::AnchorFrom(origin, ahead, up, settings.unitsPerMetre);
}

Eye PlaceEye(const rr::xr::EyeView& recentredEye, const rr::xr::WorldAnchor& anchor, float nearZ, float farZ) {
    Eye e;
    e.world = rr::xr::PlaceInWorld(recentredEye.pose, anchor);
    e.fov = recentredEye.fov;
    e.proj = rr::xr::ProjectionFromFov(recentredEye.fov, nearZ, farZ);
    return e;
}

rr::render::Mat4 UnionViewProj(const Eye eyes[2], float nearZ, float farZ) {
    float mid[3], sep[3];
    for (int k = 0; k < 3; ++k) {
        mid[k] = 0.5f * (eyes[0].world.eye[k] + eyes[1].world.eye[k]);
        sep[k] = 0.5f * (eyes[1].world.eye[k] - eyes[0].world.eye[k]);
    }
    const float half = std::sqrt(Dot(sep, sep));
    const float tl = std::min(std::tan(eyes[0].fov.left), std::tan(eyes[1].fov.left));
    const float tr = std::max(std::tan(eyes[0].fov.right), std::tan(eyes[1].fov.right));
    const float tu = std::max(std::tan(eyes[0].fov.up), std::tan(eyes[1].fov.up));
    const float td = std::min(std::tan(eyes[0].fov.down), std::tan(eyes[1].fov.down));
    // the apex back along the view so that a side plane through it passes outside the eye on that side
    const float minSide = std::max(0.05f, std::min(std::fabs(tl), std::fabs(tr)));
    const float back = half / minSide + 0.01f;
    const float* fwd = eyes[0].world.forward;
    float apex[3], target[3];
    for (int k = 0; k < 3; ++k) {
        apex[k] = mid[k] - fwd[k] * back;
        target[k] = apex[k] + fwd[k];
    }
    rr::xr::Fov fov;
    fov.left = std::atan(tl);
    fov.right = std::atan(tr);
    fov.up = std::atan(tu);
    fov.down = std::atan(td);
    const rr::render::Mat4 view = rr::render::LookAt(apex, target, eyes[0].world.up);
    return rr::render::Multiply(rr::xr::ProjectionFromFov(fov, nearZ + back, farZ + back), view);
}

float AnchorTurnRate(const rr::xr::WorldAnchor& before, const rr::xr::WorldAnchor& now, double seconds) {
    if (seconds <= 1e-4) return 0.0f;
    const float c = std::clamp(Dot(before.ahead, now.ahead), -1.0f, 1.0f);
    return static_cast<float>(std::acos(c) / seconds);
}

} // namespace rrgame::vr
