// The OpenXR display of the VR host (game_host_vr.h): rr::xr::GlSession (platform/xr/xr_session_gl.h) on the GL
// context the host made current - the desktop window's WGL context (rrgame --vr) or the Quest's EGL context.
#include "game_host_vr.h"

#include "render/gl_api.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace rrgame {

namespace {

class XrDisplay final : public VrDisplay {
public:
    XrDisplay(const VrHostConfig& config, const VrSettings& settings) {
        rr::xr::SessionOptions options;
        options.appName = "Road Rash: Jailbreak VR";
#ifdef _WIN32
        options.binding.hdc = wglGetCurrentDC();
        options.binding.hglrc = wglGetCurrentContext();
#else
        options.binding.eglDisplay = config.eglDisplay;
        options.binding.eglConfig = config.eglConfig;
        options.binding.eglContext = config.eglContext;
        options.androidVm = config.androidVm;
        options.androidActivity = config.androidActivity;
#endif
        options.eyeScale = static_cast<float>(settings.eyeScale) / 100.0f;
        options.refreshHz = static_cast<float>(settings.refreshHz);
        options.eyeSamples = settings.msaa;
        options.foveation = settings.foveation;
        // single-pass stereo (render/multiview.h); DEVELOPMENT: RRJB_VR_MULTIVIEW=0 keeps a pass per eye
        const char* mvEnv = std::getenv("RRJB_VR_MULTIVIEW");
        options.multiview = settings.multiview && (mvEnv == nullptr || std::strcmp(mvEnv, "0") != 0);
        // the theatre: 1280 x 960 (4:3, the shell's picture and the HUD are 4:3), 3.0 m wide, 2.6 m ahead
        options.quadWidth = 1280;
        options.quadHeight = 960;
        options.quadMetres = 3.0f;
        options.quadDistance = 2.6f;
        session_ = std::make_unique<rr::xr::GlSession>(options);
        (void)config;
    }
    ~XrDisplay() override {
        // A clean end: ask the runtime to end the session and walk its state machine down (at most 3 s).
        if (session_ && session_->Running()) {
            session_->RequestExit();
            for (int i = 0; i < 300 && session_->PollEvents() && session_->Running(); ++i)
                if (session_->BeginFrame()) session_->EndFrame();
            std::printf("xr: session %s after the exit request, %lld frames\n", session_->StateName(), session_->FrameIndex());
        }
    }
    const char* Name() const override { return "OpenXR"; }
    bool PollEvents() override { return session_->PollEvents(); }
    bool Running() const override { return session_->Running(); }
    bool Focused() const override { return session_->Focused(); }
    const char* StateName() const override { return session_->StateName(); }
    bool BeginFrame() override {
        const bool ok = session_->BeginFrame();
        clock_ = DisplayClock{};
        if (ok) { // the frame's display clock (vr_pacing.h)
            clock_.valid = session_->PredictedDisplayTime() > 0 && session_->PredictedDisplayPeriod() > 0;
            clock_.time = session_->PredictedDisplayTime();
            clock_.period = session_->PredictedDisplayPeriod();
            clock_.loop = SteadyNowNs();
        }
        return ok;
    }
    DisplayClock Clock() const override { return clock_; }
    bool ShouldRender() const override { return session_->ShouldRender(); }
    bool LocateViews(rr::xr::EyeView eyes[2], rr::xr::Pose& head) override { return session_->LocateViews(eyes, head); }
    rr::xr::FrameTarget BindEye(int eye) override { return session_->BindEye(eye); }
    void FinishEye(int eye) override { session_->FinishEye(eye); }
    bool Multiview() const override { return session_->Multiview(); }
    rr::xr::FrameTarget BindEyes() override { return session_->BindEyes(); }
    void FinishEyes() override { session_->FinishEyes(); }
    rr::xr::FrameTarget BindQuad() override { return session_->BindQuad(); }
    void FinishQuad() override { session_->FinishQuad(); }
    void EndFrame() override { session_->EndFrame(); }
    void KeepQuad(bool on) override { session_->KeepQuad(on); }
    void RecenterQuad() override { session_->RecenterQuad(); }
    bool PollPad(rr::xr::XrPad& pad) override { return session_->PollPad(pad); }
    void Vibrate(float left, float right) override { session_->Vibrate(left, right); }
    bool LocateHands(rr::xr::HandPose hands[2]) override {
        session_->LocateHands(hands);
        return hands[0].gripValid || hands[1].gripValid;
    }
    bool TakeRecenterEvent() override { return session_->TakeRecenterEvent(); }
    std::vector<float> RefreshRates() const override { return session_->Info().refreshRates; }
    double RefreshRate() const override { return session_->RefreshRate(); }
    bool SetRefreshRate(float hz) override { return session_->SetRefreshRate(hz); }
    void SetEyeSamples(int samples) override { session_->SetEyeSamples(samples); }
    void SetFoveation(int level) override { session_->SetFoveation(level); }
    void RequestExit() override { session_->RequestExit(); }
    long long FrameIndex() const override { return session_->FrameIndex(); }
    bool Paced() const override { return true; }
    bool ReadEye(int eye, std::vector<uint8_t>& rgba, int& width, int& height) override {
        return session_->ReadEye(eye, rgba, width, height);
    }
    bool ReadQuad(std::vector<uint8_t>& rgba, int& width, int& height) override { return session_->ReadQuad(rgba, width, height); }
    std::string Describe() const override {
        const rr::xr::SessionInfo& i = session_->Info();
        char b[400];
        std::snprintf(b, sizeof(b), "OpenXR: %s on '%s', eyes %ux%u (recommended %ux%u), %s%s, %dx MSAA, %.0f Hz",
                      i.runtimeName.c_str(), i.systemName.c_str(), i.eyeWidth, i.eyeHeight, i.recommendedWidth,
                      i.recommendedHeight, i.directEyes ? "direct" : "copied", i.multiview ? ", single-pass stereo" : "",
                      i.eyeSamples, i.refreshHz);
        return b;
    }

private:
    std::unique_ptr<rr::xr::GlSession> session_;
    DisplayClock clock_;
};

} // namespace

std::unique_ptr<VrDisplay> CreateXrDisplay(const VrHostConfig& config, const VrSettings& settings) {
    return std::make_unique<XrDisplay>(config, settings);
}

} // namespace rrgame
