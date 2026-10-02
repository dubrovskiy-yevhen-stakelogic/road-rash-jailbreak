#pragma once
// The OpenXR session of the game on OpenGL: desktop GL 3.3 through
// XR_KHR_opengl_enable on Windows (PCVR: Meta Link, Virtual Desktop, SteamVR), OpenGL ES 3.2 through
// XR_KHR_opengl_es_enable on the Quest (standalone APK). Model: the sibling project's Vulkan session
// (gt2-play src\platform\xr\xr_session.cpp, MIT) - the same start-up order, state machine,
// loader handling and theatre quad, with GL swapchains instead of Vulkan images.
//
// What it owns:
//   * the loader (Windows: openxr_loader.dll opened at run time from next to the executable; Android: the loader of
//     the APK, initialised with the activity's JavaVM / Context), the instance, the HMD system;
//   * the session on the GL context the caller made current (GlBinding: the WGL DC / context, or the EGL display /
//     config / context - egl_context_android.h makes one), the LOCAL, VIEW and (when offered) STAGE spaces;
//   * one colour swapchain per eye at the runtime's recommended size x eyeScale, and one for the theatre quad;
//   * per eye an RGBA8 + depth-stencil framebuffer of our own the game draws into EXACTLY as it draws the desktop
//     window's back buffer (the renderer's post pass copies from the bound framebuffer, which an sRGB swapchain image
//     would not allow on ES); FinishEye copies it into the acquired swapchain image with the bytes unchanged, so the
//     compositor sees what the desktop shows (an sRGB swapchain whose encoding is switched off - or undone in the copy
//     when GL_EXT_sRGB_write_control is missing);
//   * the frame loop: BeginFrame = xrWaitFrame + xrBeginFrame at the runtime's pace, EndFrame = the projection layer
//     (the two eyes as located at predictedDisplayTime) and / or the quad layer on top;
//   * the Touch action set (xr_actions.h -> the PlayStation pad model of xr_pad.h), haptics, controller poses;
//   * XR_FB_display_refresh_rate (72 / 80 / 90 / 120 Hz on Quest 3) and XR_EXT_performance_settings when offered.
//
// Every failure throws std::runtime_error with the runtime's result code in words; nothing falls back silently.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "platform/xr/xr_math.h"
#include "platform/xr/xr_pad.h"

namespace rr::xr {

struct GlBinding {
    // Windows (XR_KHR_opengl_enable): the device context and WGL context of a GL 3.3+ context (render/window_win32.h).
    void* hdc = nullptr;
    void* hglrc = nullptr;
    // Android (XR_KHR_opengl_es_enable): EGLDisplay / EGLConfig / EGLContext of an ES 3.x context.
    void* eglDisplay = nullptr;
    void* eglConfig = nullptr;
    void* eglContext = nullptr;
};

struct SessionOptions {
    std::string appName = "rrjb";
    GlBinding binding;
    float refreshHz = 0;      // > 0: ask XR_FB_display_refresh_rate for this rate
    float eyeScale = 1.0f;    // eye image = recommended size x this (clamped to the runtime's maximum)
    // The theatre quad: its image size, its width in metres, and how far ahead of the head (latched yaw) it hangs.
    uint32_t quadWidth = 1280, quadHeight = 720;
    float quadMetres = 3.2f, quadDistance = 2.5f;
    float quadYawOffset = 0.0f; // radians added to the latched head yaw (a side panel)
    float quadHeightOffset = 0.0f; // metres above the head's height
    bool preferStage = false; // the STAGE space (floor at y = 0) instead of LOCAL, when the runtime has it
    // Android: ANativeActivity::vm and ::clazz (the OpenXR loader and XR_KHR_android_create_instance need them).
    void* androidVm = nullptr;
    void* androidActivity = nullptr;
    // rrgame: multisampling of the eye images (1 = off), rendering straight into the eye swapchain images
    // when the bytes can be stored unencoded (ES with GL_EXT_sRGB_write_control - the Quest; desktop GL keeps the
    // copy), and the fixed foveation level of XR_FB_foveation (0 off .. 3 high; only a direct eye is foveated).
    int eyeSamples = 1;
    bool directEyes = true;
    int foveation = 0;
    // Single-pass stereo (render/multiview.h): with direct eyes and GL_OVR_multiview2 (and, when
    // multisampled, GL_OVR_multiview_multisampled_render_to_texture), ONE eye swapchain of two array layers - layer v is
    // eye v - drawn by one pass (BindEyes / FinishEyes). Otherwise the two swapchains and a pass per eye. Off by default
    // (rrvrtest draws per eye); rrgame asks for it (vr_settings.h multiview).
    bool multiview = false;
};

struct SessionInfo {
    std::string runtimeName, systemName, graphicsApi;
    uint32_t recommendedWidth = 0, recommendedHeight = 0, maxWidth = 0, maxHeight = 0;
    uint32_t eyeWidth = 0, eyeHeight = 0; // what the eye swapchains were made with
    int64_t swapchainFormat = 0;          // GL internal format of the eye / quad swapchains
    bool srgbSwapchain = false;
    double refreshHz = 0;
    std::vector<float> refreshRates;
    bool refreshRateExtension = false, performanceExtension = false, stageSpace = false;
    bool foveationExtension = false;      // XR_FB_foveation + configuration + swapchain update state
    bool directEyes = false;              // the eyes are drawn straight into the swapchain images
    int eyeSamples = 1;                   // the multisampling in effect
    bool msaaToTexture = false;           // ES: GL_EXT_multisampled_render_to_texture (the tile resolves, no blit)
    bool multiview = false;               // single-pass stereo: one two-layer eye swapchain (SessionOptions::multiview)
};

// A framebuffer the game draws one view into: bind it with GlSession::BindEye / BindQuad.
struct FrameTarget {
    uint32_t framebuffer = 0;
    int width = 0, height = 0;
};

class GlSession {
public:
    // Creates everything up to a session that is ready to run. The GL context of `options.binding` must be current on
    // this thread (and stay current for every call below). Throws std::runtime_error on any failure.
    explicit GlSession(const SessionOptions& options);
    ~GlSession();
    GlSession(const GlSession&) = delete;
    GlSession& operator=(const GlSession&) = delete;

    const SessionInfo& Info() const { return info_; }

    // The runtime's event queue and the state machine (xrBeginSession on READY, xrEndSession on STOPPING). False once
    // the runtime told the application to quit (EXITING, LOSS_PENDING, instance loss).
    bool PollEvents();
    bool Running() const { return running_; }
    bool Focused() const;
    bool Quit() const { return quit_; }
    const char* StateName() const;
    void RequestExit();

    // One compositor frame. BeginFrame blocks in xrWaitFrame; false when the session is not running (poll events and
    // try again). Between BeginFrame and EndFrame: LocateViews, then per eye BindEye - draw - FinishEye, optionally
    // BindQuad - draw - FinishQuad, then EndFrame (always, even when ShouldRender() is false).
    bool BeginFrame();
    bool ShouldRender() const { return shouldRender_; }
    int64_t PredictedDisplayTime() const { return displayTime_; }
    int64_t PredictedDisplayPeriod() const { return displayPeriod_; }
    // The two eyes (and the head) in the session's space at the frame's predicted display time. False when the
    // runtime cannot track them this frame (the caller draws nothing and EndFrame submits no projection layer).
    bool LocateViews(EyeView eyes[2], Pose& head);

    FrameTarget BindEye(int eye); // binds the eye's framebuffer and sets the viewport to it
    void FinishEye(int eye);      // copies it into the eye's swapchain image
    // Single-pass stereo (Info().multiview): the acquired two-layer image as one multiview framebuffer, and its release.
    bool Multiview() const { return info_.multiview; }
    FrameTarget BindEyes();
    void FinishEyes();
    FrameTarget BindQuad();
    void FinishQuad();
    // Submits the frame: the projection layer when both eyes were finished this frame, the quad when it was.
    void EndFrame();

    // The quad is placed again ahead of the head at the next frame that shows it (and after a recentre).
    void RecenterQuad() { quadLatched_ = false; }
    bool SetRefreshRate(float hz);
    double RefreshRate() const { return info_.refreshHz; }
    // Where the quad hangs (the next latch uses it): width in metres, distance ahead, height above the head, yaw offset.
    void SetQuadPlacement(float metres, float distance, float heightOffset, float yawOffset);
    // Keep showing the quad in frames that did not draw it (the last released image; a paced menu, an idle frame).
    void KeepQuad(bool on) { keepQuad_ = on; }
    // True once after the runtime recentred its space (XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING).
    bool TakeRecenterEvent() {
        const bool r = recenterEvent_;
        recenterEvent_ = false;
        return r;
    }
    // Multisampling of the eyes (1, 2, 4; clamped to the driver's), re-creating their buffers; the fixed foveation
    // level (0..3; XR_FB_foveation, only on direct eyes). Both between frames.
    void SetEyeSamples(int samples);
    void SetFoveation(int level);
    // The eye's / the quad's picture as RGBA8, bottom row first (a shot): call it between BindEye and FinishEye (a
    // direct eye is only readable while its image is acquired). False when there is nothing to read.
    bool ReadEye(int eye, std::vector<uint8_t>& rgba, int& width, int& height);
    bool ReadQuad(std::vector<uint8_t>& rgba, int& width, int& height);

    // Input (xr_actions.h). Poll once per frame after BeginFrame; the pad is released unless FOCUSED.
    bool PollPad(XrPad& pad);
    void Vibrate(float left, float right);
    void LocateHands(HandPose hands[2]);

    // Frame statistics, printed every 5 s by EndFrame: frames, the display period and the time spent waiting.
    long long FrameIndex() const { return frameIndex_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    SessionOptions options_;
    SessionInfo info_;
    bool running_ = false, quit_ = false, shouldRender_ = false, frameOpen_ = false, quadLatched_ = false;
    bool keepQuad_ = false, recenterEvent_ = false;
    int state_ = 0; // XrSessionState
    int64_t displayTime_ = 0, displayPeriod_ = 0;
    long long frameIndex_ = 0;
};

} // namespace rr::xr
