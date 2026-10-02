#pragma once
// The VR head view's visual lean (after GTA SA VR's BikeVisualLeanPercent: the gta-sa-vr-quest project,
// native\src\Driving.cpp, the lean matrix recomputed with lean x percent for the model / camera / hands only). The
// player's own bike is DRAWN with a percentage of the original's roll, so with the horizon lock the drawn handlebars stay
// near the player's real hands; the simulation (+0x1B0 in the guest) is never touched.
//
// The roll: the bike's up (the model's -y, the rows +0x1B0 as race_render.cpp RecordModelMatrix takes them) against the
// level plane through its forward axis, signed about forward. The drawn bike, the grips, the held hands and the head
// camera are all turned by -(1 - scale) x roll about ONE axis - the bike's forward through its wheels' contact point
// (the origin +0xB8 moved down the bike's up by the rest model's lowest point, BarGrips::ContactUp) - so the wheels stay
// on the road and the eye stays over the seat. Header only (no GL): race_render.cpp, vr_bar_grips.cpp and main.cpp.
#include "render/mat4.h"

#include <algorithm>
#include <cmath>

namespace rrgame {

struct VisualLean {
    bool on = false;                 // a rotation to apply (scale < 1 and a valid frame)
    float point[3] = {};             // a point of the axis (the contact point, world)
    float axis[3] = {0, 0, 1};       // the bike's forward (unit, world)
    float angle = 0.0f;              // radians about `axis` (right-handed in the world's own axes)
    float rollDegrees = 0.0f;        // the original's roll (the log)
    // a second turn after the lean - the wheelie's drawn pitch (src\game\wheelie.h) about `pitchAxis` (the
    // leaned bike's right) through `pitchPoint` (its rear wheel's contact point). `on` is set whenever either turn is
    // (a pitch alone leaves `angle` 0: the lean's turn is then the identity).
    bool pitchOn = false;
    float pitchPoint[3] = {};
    float pitchAxis[3] = {1, 0, 0};
    float pitchAngle = 0.0f;

    // From a bike model matrix (column-major: col 0 the model's x - right -, col 1 its y - down -, col 2 its z -
    // forward, col 3 the origin), `scale` 0..1 of the roll kept, `contactUp` world units along the bike's up from the
    // origin to the wheels' contact point (<= 0).
    static VisualLean From(const rr::render::Mat4& m, float scale, float contactUp) {
        VisualLean v;
        const auto len = [](const float a[3]) { return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); };
        float fwd[3] = {m.m[8], m.m[9], m.m[10]}, up[3] = {-m.m[4], -m.m[5], -m.m[6]};
        const float lf = len(fwd), lu = len(up);
        if (!(lf > 1e-9f) || !(lu > 1e-9f)) return v;
        for (int k = 0; k < 3; ++k) {
            fwd[k] /= lf;
            up[k] /= lu;
        }
        // the level up through forward: the world's up (-y) made square to forward
        float lvl[3];
        const float w[3] = {0.0f, -1.0f, 0.0f};
        const float wf = w[0] * fwd[0] + w[1] * fwd[1] + w[2] * fwd[2];
        for (int k = 0; k < 3; ++k) lvl[k] = w[k] - fwd[k] * wf;
        const float ll = len(lvl);
        if (!(ll > 1e-4f)) return v; // standing on its nose or tail: no roll to speak of
        for (float& c : lvl) c /= ll;
        const float cr[3] = {lvl[1] * up[2] - lvl[2] * up[1], lvl[2] * up[0] - lvl[0] * up[2], lvl[0] * up[1] - lvl[1] * up[0]};
        const float roll = std::atan2(cr[0] * fwd[0] + cr[1] * fwd[1] + cr[2] * fwd[2],
                                      lvl[0] * up[0] + lvl[1] * up[1] + lvl[2] * up[2]);
        v.rollDegrees = roll * 57.29577951f;
        const float s = std::clamp(scale, 0.0f, 1.0f);
        if (s >= 1.0f) return v;
        v.on = true;
        v.angle = -(1.0f - s) * roll;
        for (int k = 0; k < 3; ++k) {
            v.axis[k] = fwd[k];
            v.point[k] = m.m[12 + k] + up[k] * contactUp;
        }
        return v;
    }
    // the bike drawn at `drawnRoll` (radians, the sign of `rollDegrees`) whatever the original's own roll -
    // the Modern handling's lean (src\game\handling_modern.h); the same axis and point as From.
    static VisualLean ToRoll(const rr::render::Mat4& m, float drawnRoll, float contactUp) {
        VisualLean v = From(m, 0.0f, contactUp); // scale 0: on, the axis and the point set, angle = -roll
        if (v.on) v.angle = drawnRoll - v.rollDegrees / 57.29577951f;
        return v;
    }
    // Rodrigues about a unit axis.
    static void Rotate(const float ax[3], float ang, const float in[3], float out[3]) {
        const float c = std::cos(ang), s = std::sin(ang);
        const float d = ax[0] * in[0] + ax[1] * in[1] + ax[2] * in[2];
        const float x[3] = {ax[1] * in[2] - ax[2] * in[1], ax[2] * in[0] - ax[0] * in[2], ax[0] * in[1] - ax[1] * in[0]};
        float o[3];
        for (int k = 0; k < 3; ++k) o[k] = in[k] * c + x[k] * s + ax[k] * d * (1.0f - c);
        for (int k = 0; k < 3; ++k) out[k] = o[k];
    }
    // The lean about `axis`, then the pitch about `pitchAxis`.
    void Dir(const float in[3], float out[3]) const {
        if (!on) {
            for (int k = 0; k < 3; ++k) out[k] = in[k];
            return;
        }
        Rotate(axis, angle, in, out);
        if (pitchOn) Rotate(pitchAxis, pitchAngle, out, out);
    }
    void Point(const float in[3], float out[3]) const {
        if (!on) {
            for (int k = 0; k < 3; ++k) out[k] = in[k];
            return;
        }
        float r[3] = {in[0] - point[0], in[1] - point[1], in[2] - point[2]};
        Rotate(axis, angle, r, r);
        for (int k = 0; k < 3; ++k) out[k] = point[k] + r[k];
        if (pitchOn) {
            for (int k = 0; k < 3; ++k) r[k] = out[k] - pitchPoint[k];
            Rotate(pitchAxis, pitchAngle, r, r);
            for (int k = 0; k < 3; ++k) out[k] = pitchPoint[k] + r[k];
        }
    }
    // add the wheelie's pitch (radians, nose up > 0) after the lean: about the bike's right axis through
    // its rear wheel's contact point - `contactUp` along the bike's up from the origin (as From), `rearBack` along its
    // forward behind the origin - both as the lean leaves them. `m` is the unleaned model matrix (as From takes it).
    void AddPitch(const rr::render::Mat4& m, float pitch, float contactUp, float rearBack) {
        const auto len = [](const float a[3]) { return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); };
        float right[3] = {m.m[0], m.m[1], m.m[2]}, fwd[3] = {m.m[8], m.m[9], m.m[10]}, up[3] = {-m.m[4], -m.m[5], -m.m[6]};
        const float lr = len(right), lf = len(fwd), lu = len(up);
        if (!(lr > 1e-9f) || !(lf > 1e-9f) || !(lu > 1e-9f) || !(pitch != 0.0f)) return;
        for (int k = 0; k < 3; ++k) {
            right[k] /= lr;
            fwd[k] /= lf;
            up[k] /= lu;
        }
        float rear[3];
        for (int k = 0; k < 3; ++k) rear[k] = m.m[12 + k] + up[k] * contactUp - fwd[k] * rearBack;
        // the sign that lifts the nose: forward turned about right toward up
        const float rf[3] = {right[1] * fwd[2] - right[2] * fwd[1], right[2] * fwd[0] - right[0] * fwd[2],
                             right[0] * fwd[1] - right[1] * fwd[0]};
        const float sign = rf[0] * up[0] + rf[1] * up[1] + rf[2] * up[2] >= 0.0f ? 1.0f : -1.0f;
        const bool wasOn = on;
        pitchOn = false; // (the lean alone takes the point and the axis)
        on = true;

        if (!wasOn) angle = 0.0f; // (From / ToRoll left the axis and the point; the lean's turn is the identity)
        Point(rear, pitchPoint);
        Dir(right, pitchAxis);
        pitchAngle = sign * pitch;
        pitchOn = true;
    }

    // A model matrix: its three axes turned, its origin turned about the axis.
    void Matrix(rr::render::Mat4& m) const {
        if (!on) return;
        for (int c = 0; c < 3; ++c) Dir(&m.m[4 * c], &m.m[4 * c]);
        Point(&m.m[12], &m.m[12]);
    }
};

} // namespace rrgame
