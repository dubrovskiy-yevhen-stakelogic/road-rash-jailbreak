#pragma once
// The VR host (game_host.h): the whole game in a headset.
//
//   * The display: an OpenXR session on the game's own GL context (XR_KHR_opengl_enable on Windows - rrgame --vr,
//     RRJB_XR_RUNTIME=meta | steamvr | vdxr | system as the PLAY-PCVR-*.bat launchers set it -, XR_KHR_opengl_es_enable
//     on the Quest) or the desktop VR MOCK (rrgame --vr-mock: the same frames into offscreen eyes with a synthetic
//     head, no headset - the verification path; hidden and silent in a scripted run).
//   * The race: a stereo projection layer. Per eye race_render.h RenderView with consoleCamera = false and the eye's
//     own asymmetric projection, anchored on the rider's head (game/head_camera.h) or the original's chase camera,
//     horizon-locked (vr_rig.h); the original HUD on a panel fixed to the levelled seat (vr_draw.h).
//   * Everything flat - the front end's menus, the films, the loading and results screens, the VR settings menu - on
//     the theatre quad ahead of the player (xr_session_gl.h), the shell paced at its own 60 Hz while the compositor
//     runs at the display's rate (the last image shown again between the shell's frames).
//   * Input: the Touch controllers by position into PhysicalPad and the VR bindings (input_bindings.h [vr_controls]);
//     in the menus A confirms, B goes back; the rumble on the Touch haptics. On Windows the desktop window's keyboard
//     and a desktop controller work as well.
#include "game_host.h"
#include "graphics_settings.h"
#include "platform/xr/xr_math.h"
#include "platform/xr/xr_pad.h"
#include "platform/xr/xr_session_gl.h" // FrameTarget, SessionInfo (no OpenXR types)
#include "vr_pacing.h"   // DisplayClock
#include "vr_rig.h"
#include "vr_settings.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace rrgame {

// The display behind the VR host: an OpenXR session (game_host_vr_xr.cpp) or the desktop mock (game_host_vr.cpp).
class VrDisplay {
public:
    virtual ~VrDisplay() = default;
    virtual const char* Name() const = 0;
    virtual bool PollEvents() = 0; // false: the runtime asked the application to quit
    virtual bool Running() const = 0;
    virtual bool Focused() const = 0;
    virtual const char* StateName() const = 0;
    virtual bool BeginFrame() = 0; // blocks for the display's frame; false when the session is not running
    virtual bool ShouldRender() const = 0;
    virtual bool LocateViews(rr::xr::EyeView eyes[2], rr::xr::Pose& head) = 0;
    virtual rr::xr::FrameTarget BindEye(int eye) = 0;
    virtual void FinishEye(int eye) = 0;
    // Single-pass stereo (render/multiview.h): both eyes are the two layers of one framebuffer - BindEyes, one pass,
    // FinishEyes, instead of BindEye / FinishEye per eye. Decided when the display is made (the setting, the GPU).
    virtual bool Multiview() const { return false; }
    virtual rr::xr::FrameTarget BindEyes() { return {}; }
    virtual void FinishEyes() {}
    virtual rr::xr::FrameTarget BindQuad() = 0;
    virtual void FinishQuad() = 0;
    virtual void EndFrame() = 0;
    virtual void KeepQuad(bool on) = 0;
    virtual void RecenterQuad() = 0;
    virtual bool PollPad(rr::xr::XrPad& pad) = 0;
    virtual void Vibrate(float left, float right) = 0;
    // The controllers' grip / aim poses at this frame's display time, in the display's space (vr_handlebars.h).
    virtual bool LocateHands(rr::xr::HandPose hands[2]) {
        hands[0] = hands[1] = rr::xr::HandPose{};
        return false;
    }
    virtual bool TakeRecenterEvent() = 0;
    virtual std::vector<float> RefreshRates() const = 0;
    virtual double RefreshRate() const = 0;
    virtual bool SetRefreshRate(float hz) = 0;
    virtual void SetEyeSamples(int samples) = 0;
    virtual void SetFoveation(int level) = 0;
    virtual void RequestExit() = 0;
    virtual long long FrameIndex() const = 0;
    virtual bool Paced() const = 0; // the frames wait for a display (false: the mock)
    // A shot of the eye / the quad (RGBA8, bottom row first): between BindEye / BindQuad and their Finish.
    virtual bool ReadEye(int eye, std::vector<uint8_t>& rgba, int& width, int& height) = 0;
    virtual bool ReadQuad(std::vector<uint8_t>& rgba, int& width, int& height) = 0;
    virtual std::string Describe() const = 0;
    // The clock of the frame the last BeginFrame opened (vr_pacing.h) - xrWaitFrame's predictedDisplayTime /
    // predictedDisplayPeriod (the mock: its synthetic clock with --vr-mock-hz / --vr-mock-timing; invalid otherwise)
    virtual DisplayClock Clock() const { return {}; }
    // The mock's "hold" (--vr-mock-timing): the host held the game, as Block does on a headset.
    virtual bool TakeHeld() { return false; }
};

// What the platform hands the VR host (main_desktop.cpp, main_android.cpp).
struct VrHostConfig {
    bool mock = false;             // the desktop VR mock instead of OpenXR
    bool scripted = false;         // a scripted run (a frame count, a shot): defaults, no settings file, no pacing
    // the mock's synthetic head: yaw / pitch / roll in degrees, the eye image size
    float mockYaw = 0.0f, mockPitch = 0.0f, mockRoll = 0.0f;
    float mockPos[3] = {0.0f, 0.0f, 0.0f}; // the head moved off the seat, metres (OpenXR: x right, y up, z back)
    int mockEyeWidth = 960, mockEyeHeight = 1008;
    int menuShotPage = -1;         // DEVELOPMENT: the VR menu drawn open on this page (the mock's shots)
    // Android: the EGL handles, the activity's JavaVM / instance, and the looper pump (false: the activity is going)
    void* eglDisplay = nullptr;
    void* eglConfig = nullptr;
    void* eglContext = nullptr;
    void* androidVm = nullptr;
    void* androidActivity = nullptr;
    std::function<bool(int timeoutMs)> platformPump;
};

// One stereo frame's placement (BeginStereo): both eyes, the anchor and the union frustum they are culled with.
struct VrStereo {
    vr::Eye eyes[2];
    rr::xr::WorldAnchor anchor;
    rr::render::Mat4 cullViewProj;
    float nearZ = 0.05f, farZ = 20000.0f;
    int width = 0, height = 0; // one eye's image
};

class VrHost : public GameHost {
public:
    virtual VrDisplay& Display() = 0;
    virtual const VrHostConfig& Config() const = 0;

    // ---- the frame
    // Waits for the display's frame (xrWaitFrame + xrBeginFrame). False when there is no frame this turn (the session
    // is not running: the host has already waited). Every true BeginFrame is followed by EndFrame.
    virtual bool BeginFrame() = 0;
    virtual bool ShouldRender() const = 0;
    // The eyes of this frame placed on `camera` (vr_rig.h: the recentre, the horizon lock, the seat). False when the
    // runtime cannot locate the head this frame (draw nothing then; EndFrame submits no projection layer).
    virtual bool LocateStereo(const vr::Camera& camera, bool lookBack, float farZ, VrStereo& out) = 0;
    virtual rr::xr::FrameTarget BindEye(int eye) = 0;
    virtual void FinishEye(int eye) = 0;
    // Single-pass stereo (VrDisplay::Multiview): one framebuffer whose two layers are the eyes.
    virtual bool Multiview() const = 0;
    virtual rr::xr::FrameTarget BindEyes() = 0;
    virtual void FinishEyes() = 0;
    virtual rr::xr::FrameTarget BindQuad() = 0;
    virtual void FinishQuad() = 0;
    virtual void EndFrame() = 0;
    // One frame of the theatre quad: `draw(w, h)` fills the bound quad image (the shell's picture); the VR menu goes
    // over it when open. Returns false when no frame was shown (not running).
    virtual bool TheatreFrame(const std::function<void(int, int)>& draw) = 0;
    // A frame that shows the last quad image again (the shell's 60 Hz between the display's frames, a long load).
    virtual void IdleFrame() = 0;
    // The race draws the menu itself into the quad inside its frame (DrawMenuQuad between BeginFrame and EndFrame).
    virtual void DrawMenuQuad() = 0;
    // The seconds to the next display frame's time at the display's rate (for the shell's pacing): the display period.
    virtual double DisplayPeriod() const = 0;
    // the clock of the frame the last BeginFrame opened (vr_pacing.h; invalid: none known)
    virtual DisplayClock FrameClock() const = 0;

    // ---- the menu and the recentre
    // Once a loop turn after Pump: the menu's chord (L3 + R3, or both grips + Menu; F10 on a keyboard) opens it; its
    // input is taken while it is open. True while it holds the game (open, or its buttons not yet let go).
    virtual bool MenuHolds() = 0;
    virtual bool MenuOpen() const = 0;
    virtual void RequestRecentre() = 0;
    // True once after the host blocked (headset off, session not focused): the loop restarts its time step.
    virtual bool TakeTimingReset() = 0;
    // The race's sound: muted while the host blocks or the menu holds (the race's MixerSink::muted).
    virtual void SetAudioMute(std::atomic<bool>* muted) = 0;
    // The race asks, per frame, whether the player pressed the view button (the camera action) and whether it is held.
    virtual VrSettings& Settings() = 0;
    virtual void SaveSettings() = 0;
    // The PC graphics settings in VR: no console look (no dither, 24-bit, perspective-correct, the depth buffer),
    // detail / distance / textures from the VR settings, no offscreen target (the eyes are the target), no FPS cap.
    virtual void ApplyGraphics(GraphicsSettings& gfx) = 0;
    virtual vr::RigSettings Rig() const = 0;

    // ---- shots (the mock's evidence; the Quest's files dir)
    // The next frame's eyes (and the quad when drawn) as one PNG: eye 0 | eye 1, the quad below.
    virtual void RequestShot(const std::string& path) = 0;
    virtual bool ShotDone() const = 0;

    // ---- the hands (vr_handlebars.h)
    // This turn's Touch controllers, unmapped (Pump polled them).
    virtual const rr::xr::XrPad& TouchState() const = 0;
    // The hands' grip / aim poses in the recentred seat's space (the eyes' own recentre, vr_rig.h), metres.
    virtual bool LocateHands(rr::xr::HandPose hands[2]) = 0;
    // The hands' own haptics (0..1 per hand, the vibration setting applied), merged with the game's rumble.
    virtual void SetHandHaptics(float left, float right) = 0;
    // the race says, every step, whether it is racing (game state 1: not paused, not the results). The
    // Touch haptics are sent only then, and never while the VR menu is open or held, or the session is not focused -
    // the actuators are stopped at once when any of these starts (RRJB_VR_HAPTICS_HOLD=off: the control).
    virtual void SetHapticsRacing(bool racing) = 0;
};

// The VR host of this process (main_desktop.cpp / main_android.cpp create it and pass it to SetHost).
std::unique_ptr<VrHost> CreateVrHost(const VrHostConfig& config);
// The OpenXR display on the current GL context (game_host_vr_xr.cpp); throws with the runtime's words on failure.
std::unique_ptr<VrDisplay> CreateXrDisplay(const VrHostConfig& config, const VrSettings& settings);

} // namespace rrgame
