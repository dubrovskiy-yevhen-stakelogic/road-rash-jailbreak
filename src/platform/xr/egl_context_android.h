#pragma once
// The OpenGL ES 3.2 context of the Quest build: an EGL display, an RGBA8 config and a
// context current on a 16 x 16 pbuffer (the game never draws to an EGL surface - every view goes into the OpenXR
// swapchains through framebuffers, xr_session_gl.h). The three handles are what XR_KHR_opengl_es_enable binds.
namespace rr::xr {

class EglContext {
public:
    EglContext();  // creates it and makes it current on this thread; throws std::runtime_error on failure
    ~EglContext();
    EglContext(const EglContext&) = delete;
    EglContext& operator=(const EglContext&) = delete;
    void* Display() const { return display_; }
    void* Config() const { return config_; }
    void* Context() const { return context_; }

private:
    void* display_ = nullptr;
    void* config_ = nullptr;
    void* context_ = nullptr;
    void* surface_ = nullptr;
};

} // namespace rr::xr
