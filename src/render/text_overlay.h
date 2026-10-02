#pragma once
// Screen-space text and panels for the PC overlays (the F10 settings overlay, the frame profiler), drawn at the
// window's own resolution over the finished frame. The letters come from a font atlas rendered at run time from a
// Windows system font (Consolas) - no game font or artwork is involved.
#include "render/gl_api.h"

#include <string>
#include <vector>

namespace rr::render {

class TextOverlay {
public:
    // A GL context must be current; builds the program and the atlas once. False on failure.
    bool Init();
    // Starts a batch over the whole w x h framebuffer that is bound (pixels, top-left origin).
    void Begin(int width, int height);
    void Rect(float x, float y, float w, float h, float r, float g, float b, float a);
    // `scale` 1 = the atlas's 24-pixel line.
    void Text(float x, float y, const std::string& s, float r, float g, float b, float a = 1.0f, float scale = 1.0f);
    float TextWidth(const std::string& s, float scale = 1.0f) const;
    float LineHeight(float scale = 1.0f) const { return 24.0f * scale; }
    // Draws the batch (blended, no depth test) and restores the depth test.
    void End();
    bool Ready() const { return program_ != 0; }

private:
    struct Vertex {
        float x, y, u, v, r, g, b, a;
    };
    void Quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, float r, float g, float b, float a);
    GLuint program_ = 0, vao_ = 0, vbo_ = 0, atlas_ = 0;
    GLint screenLocation_ = -1, atlasLocation_ = -1;
    int width_ = 1, height_ = 1;
    std::vector<Vertex> vertices_;
    float advance_[96] = {};
};

} // namespace rr::render
