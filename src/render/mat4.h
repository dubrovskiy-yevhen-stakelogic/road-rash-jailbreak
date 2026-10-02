#pragma once
// The four matrix operations the renderer needs. Column-major, the layout glUniformMatrix4fv
// expects with `transpose = GL_FALSE`.
namespace rr::render {

struct Mat4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

Mat4 Multiply(const Mat4& a, const Mat4& b);
Mat4 Perspective(float fovYRadians, float aspect, float nearZ, float farZ);
Mat4 LookAt(const float eye[3], const float target[3], const float up[3]);

} // namespace rr::render
