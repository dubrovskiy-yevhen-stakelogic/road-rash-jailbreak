// The VR host (game_host_vr.h): the frame, the theatre, the Touch controllers, the VR menu, and the desktop mock.
#include "game_host_vr.h"

#include "platform/input_bindings.h"
#include "platform/xr/haptic_pulse.h"
#include "render/frame_shot.h"
#include "render/gl_api.h"
#include "render/multiview.h"
#include "render/render_target.h"
#include "render/text_overlay.h"
#include "vr_comfort.h"
#include "settings_file.h" // SettingsMarkersOff
#include "vr_menu.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace rrgame {

namespace {

using Clock = std::chrono::steady_clock;

// DEVELOPMENT: RRJB_VR_MULTIVIEW=0 keeps one pass per eye whatever the setting says (an A/B control).
bool MultiviewAllowed() {
    const char* e = std::getenv("RRJB_VR_MULTIVIEW");
    return e == nullptr || std::strcmp(e, "0") != 0;
}

// ---------------------------------------------------------------- the desktop mock
// The VR frame without a headset: two offscreen eyes and an offscreen quad, a synthetic seated head (at the LOCAL
// origin, 64 mm between the eyes, a Quest-3-like asymmetric field - rrvrtest's MockEyes), turned by the configured
// yaw / pitch. Its frames are not paced by a display (an interactive mock sleeps to 72 Hz).
class MockDisplay final : public VrDisplay {
public:
    MockDisplay(const VrHostConfig& config, const VrSettings& settings) : config_(config) {
        const float scale = static_cast<float>(settings.eyeScale) / 100.0f;
        eyeW_ = std::max(64, static_cast<int>(std::lround(config.mockEyeWidth * scale)));
        eyeH_ = std::max(64, static_cast<int>(std::lround(config.mockEyeHeight * scale)));
        samples_ = settings.msaa;
        for (rr::render::RenderTarget& t : eyes_)
            if (!t.Ensure(eyeW_, eyeH_, samples_)) throw std::runtime_error("vr mock: no offscreen eye target");
        if (!quad_.Ensure(1280, 960, 1)) throw std::runtime_error("vr mock: no offscreen quad target");
        // single-pass stereo (render/multiview.h): the two eyes as the layers of one target, when the GPU has it
        if (settings.multiview && MultiviewAllowed()) multiview_ = mv_.Ensure(eyeW_, eyeH_, samples_);
        std::printf("vr mock: two %dx%d eyes (%dx MSAA), %s, a 1280x960 quad, a synthetic head at yaw %.1f pitch %.1f "
                    "deg - no headset\n", eyeW_, eyeH_, multiview_ ? mv_.Samples() : eyes_[0].Samples(),
                    multiview_ ? "single-pass stereo (GL_OVR_multiview2, two layers)" : "one pass per eye",
                    double(config.mockYaw), double(config.mockPitch));
    }
    const char* Name() const override { return "the desktop VR mock"; }
    bool PollEvents() override { return true; }
    bool Running() const override { return true; }
    bool Focused() const override { return true; }
    const char* StateName() const override { return "MOCK"; }
    bool BeginFrame() override {
        if (!config_.scripted) { // an interactive mock looks at the picture at a headset's 72 Hz
            next_ += std::chrono::microseconds(13889);
            const auto now = Clock::now();
            if (next_ < now - std::chrono::milliseconds(50)) next_ = now;
            std::this_thread::sleep_until(next_);
        }
        quadDrawn_ = false;
        // a synthetic display clock with --vr-mock-hz / --vr-mock-timing (vr_pacing.h)
        clock_ = MockDisplayHz() > 0.0 ? ProductMockClock().Next(MockDisplayHz()) : DisplayClock{};
        return true;
    }
    DisplayClock Clock() const override { return clock_; }
    bool TakeHeld() override { return ProductMockClock().TakeHeld(); }
    bool ShouldRender() const override { return true; }
    bool LocateViews(rr::xr::EyeView eyes[2], rr::xr::Pose& head) override {
        const float yaw = config_.mockYaw * 3.14159265f / 180.0f, pitch = config_.mockPitch * 3.14159265f / 180.0f;
        // q = Ry(yaw) * Rx(pitch): turn left for a positive yaw (OpenXR: y up, -z ahead), look up for a positive pitch
        const float cy = std::cos(yaw * 0.5f), sy = std::sin(yaw * 0.5f), cp = std::cos(pitch * 0.5f), sp = std::sin(pitch * 0.5f);
        const float p[4] = {cy * sp, sy * cp, -sy * sp, cy * cp};
        // then the roll about the view axis (DEVELOPMENT, --vr-mock-roll: a tilted head; positive = the head tips left):
        // q = p * Rz(roll)
        const float roll = config_.mockRoll * 3.14159265f / 180.0f, sr = std::sin(roll * 0.5f), cr = std::cos(roll * 0.5f);
        const float q[4] = {p[0] * cr + p[1] * sr, p[1] * cr - p[0] * sr, p[2] * cr + p[3] * sr, p[3] * cr - p[2] * sr};
        head = rr::xr::Pose{};
        for (int k = 0; k < 4; ++k) head.orientation[k] = q[k];
        for (int k = 0; k < 3; ++k) head.position[k] = config_.mockPos[k];
        float m[9];
        rr::xr::QuatToMatrix3(q, m);
        for (int e = 0; e < 2; ++e) {
            eyes[e] = rr::xr::EyeView{};
            const float half = e == 0 ? -0.032f : 0.032f;
            for (int k = 0; k < 3; ++k) eyes[e].pose.position[k] = m[k] * half + config_.mockPos[k]; // along the head's right axis
            for (int k = 0; k < 4; ++k) eyes[e].pose.orientation[k] = q[k];
        }
        eyes[0].fov = {-0.9f, 0.75f, 0.8f, -0.85f};
        eyes[1].fov = {-0.75f, 0.9f, 0.8f, -0.85f};
        return true;
    }
    rr::xr::FrameTarget BindEye(int eye) override {
        const rr::render::RenderTarget& t = eyes_[eye & 1];
        t.Bind();
        glViewport(0, 0, t.Width(), t.Height());
        return {t.DrawFramebuffer(), t.Width(), t.Height()};
    }
    void FinishEye(int eye) override {
        eyes_[eye & 1].Resolve();
        rr::render::BindFramebuffer(0);
    }
    bool Multiview() const override { return multiview_; }
    rr::xr::FrameTarget BindEyes() override {
        mv_.Bind();
        return {mv_.Framebuffer(), mv_.Width(), mv_.Height()};
    }
    void FinishEyes() override {
        mv_.Resolve();
        mv_.DiscardDepth();
        rr::render::BindFramebuffer(0);
    }
    rr::xr::FrameTarget BindQuad() override {
        quad_.Bind();
        glViewport(0, 0, quad_.Width(), quad_.Height());
        return {quad_.DrawFramebuffer(), quad_.Width(), quad_.Height()};
    }
    void FinishQuad() override {
        quadDrawn_ = true;
        rr::render::BindFramebuffer(0);
    }
    // The mock's timing (the Quest's GPU without a session, the desktop's): every frame is finished (glFinish), so the
    // time from one BeginFrame to the next is the whole frame's CPU + GPU work; a line every 5 s.
    void EndFrame() override {
        ++frames_;
        const auto submitted = Clock::now();
        glFinish();
        const auto now = Clock::now();
        windowWaitMs_ += std::chrono::duration<double, std::milli>(now - submitted).count();
        if (lastEnd_ != Clock::time_point{}) {
            const double ms = std::chrono::duration<double, std::milli>(now - lastEnd_).count();
            windowMs_ += ms;
            worstMs_ = std::max(worstMs_, ms);
            ++windowFrames_;
        }
        lastEnd_ = now;
        if (windowStart_ == Clock::time_point{}) windowStart_ = now;
        if (std::chrono::duration<double>(now - windowStart_).count() >= 5.0 && windowFrames_ > 0) {
            std::printf("vr mock timing: %ld frames, %.2f ms a frame on average (%.1f fps), worst %.2f ms, of which %.2f ms "
                        "waiting for the GPU at the frame's end - two %dx%d eyes, %dx MSAA%s, CPU + GPU (glFinish each frame)\n",
                        windowFrames_, windowMs_ / double(windowFrames_), 1000.0 * double(windowFrames_) / windowMs_,
                        worstMs_, windowWaitMs_ / double(windowFrames_), eyeW_, eyeH_,
                        multiview_ ? mv_.Samples() : eyes_[0].Samples(), multiview_ ? ", single-pass stereo" : "");
            windowStart_ = now;
            windowFrames_ = 0;
            windowMs_ = worstMs_ = windowWaitMs_ = 0.0;
        }
    }
    void KeepQuad(bool) override {}
    void RecenterQuad() override {}
    bool PollPad(rr::xr::XrPad& pad) override {
        pad = rr::xr::XrPad{};
        pad.Derive();
        return false;
    }
    // No actuator: the haptics policy of the real session (haptic_pulse.h) on the mock's own 72 Hz frame
    // clock, counted - what the Touch controllers would have been sent
    void Vibrate(float left, float right) override {
        const int64_t now = frames_ * 1'000'000'000LL / 72;
        const float amp[2] = {left, right};
        ++vibrateCalls_;
        rr::xr::HapticCommand c[2];
        for (int h = 0; h < 2; ++h) c[h] = pulse_[h].Update(amp[h], now);
        static std::FILE* hapticLog = [] { // RRJB_HAPTICS_LOG=<csv>: every call and what it sent (a measurement)
            const char* p = std::getenv("RRJB_HAPTICS_LOG");
            std::FILE* f = p ? std::fopen(p, "wb") : nullptr;
            if (f) std::fprintf(f, "frame,left,right,sentLeft,sentRight\n");
            return f;
        }();
        if (hapticLog) {
            static const char kSent[] = {'-', 'A', 'S'};
            std::fprintf(hapticLog, "%lld,%.4f,%.4f,%c,%c\n", frames_, double(left), double(right), kSent[c[0].kind],
                         kSent[c[1].kind]);
            std::fflush(hapticLog);
        }
    }
    // what the actuators were sent so far, both hands (the haptics accounting)
    uint64_t HapticApplies() const { return pulse_[0].Applies() + pulse_[1].Applies(); }
    uint64_t HapticStops() const { return pulse_[0].Stops() + pulse_[1].Stops(); }
    bool TakeRecenterEvent() override { return false; }
    std::vector<float> RefreshRates() const override { return {72.0f, 80.0f, 90.0f, 120.0f}; } // what a Quest 3 offers
    double RefreshRate() const override { return clock_.valid ? ProductMockClock().Hz() : 72.0; }
    bool SetRefreshRate(float) override { return true; }
    void SetEyeSamples(int samples) override {
        samples_ = samples;
        for (rr::render::RenderTarget& t : eyes_) t.Ensure(eyeW_, eyeH_, samples_);
        if (multiview_ && !mv_.Ensure(eyeW_, eyeH_, samples_))
            throw std::runtime_error("vr mock: the two-layer eye target could not be made again");
    }
    void SetFoveation(int) override {}
    void RequestExit() override {}
    long long FrameIndex() const override { return frames_; }
    bool Paced() const override { return false; }
    bool ReadEye(int eye, std::vector<uint8_t>& rgba, int& width, int& height) override {
        if (multiview_) { // between BindEyes and FinishEyes: the layer, resolved
            mv_.Resolve();
            width = mv_.Width();
            height = mv_.Height();
            mv_.ReadLayer(eye, rgba);
            mv_.Bind();
            return true;
        }
        const rr::render::RenderTarget& t = eyes_[eye & 1];
        t.Resolve(); // binds the resolved image
        width = t.Width();
        height = t.Height();
        rgba = rr::render::ReadFrame(width, height);
        t.Bind();
        return true;
    }
    bool ReadQuad(std::vector<uint8_t>& rgba, int& width, int& height) override {
        quad_.Resolve();
        width = quad_.Width();
        height = quad_.Height();
        rgba = rr::render::ReadFrame(width, height);
        quad_.Bind();
        return true;
    }
    std::string Describe() const override {
        char b[480];
        std::snprintf(b, sizeof(b), "the desktop VR mock: eyes %dx%d, %dx MSAA, %s, head yaw %.1f pitch %.1f; haptics "
                      "(%s): %llu Vibrate call(s) -> xrApplyHapticFeedback left %llu right %llu, "
                      "xrStopHapticFeedback left %llu right %llu", eyeW_, eyeH_,
                      multiview_ ? mv_.Samples() : eyes_[0].Samples(), multiview_ ? "single-pass stereo" : "a pass per eye",
                      double(config_.mockYaw), double(config_.mockPitch),
                      pulse_[0].Legacy() ? "RRJB_HAPTICS=frame, a pulse every call" : "pulse on a change",
                      static_cast<unsigned long long>(vibrateCalls_), static_cast<unsigned long long>(pulse_[0].Applies()),
                      static_cast<unsigned long long>(pulse_[1].Applies()), static_cast<unsigned long long>(pulse_[0].Stops()),
                      static_cast<unsigned long long>(pulse_[1].Stops()));
        return b;
    }

private:
    VrHostConfig config_;
    rr::render::RenderTarget eyes_[2], quad_;
    rr::render::MultiviewTarget mv_; // single-pass stereo (Multiview)
    bool multiview_ = false;
    int eyeW_ = 0, eyeH_ = 0, samples_ = 1;
    long long frames_ = 0;
    DisplayClock clock_;
    rr::xr::HapticPulse pulse_[2];
    unsigned long long vibrateCalls_ = 0;
    bool quadDrawn_ = false;
    Clock::time_point next_ = Clock::now();
    Clock::time_point lastEnd_{}, windowStart_{};
    long windowFrames_ = 0;
    double windowMs_ = 0.0, worstMs_ = 0.0, windowWaitMs_ = 0.0;
};

// ---------------------------------------------------------------- the Touch controllers
// By position into the PhysicalPad the bindings map (input_bindings.h [vr_controls]): A Cross, B Circle, X Square,
// Y Triangle, the grips L1 / R1, the triggers L2 / R2 (with their travel), the stick clicks L3 / R3, Menu Start, the
// left stick its directions and bytes, the right stick the d-pad positions and its bytes.
rr::platform::PhysicalPad TouchToPhysical(const rr::xr::XrPad& t) {
    using rr::platform::Bit;
    using rr::platform::PadButton;
    rr::platform::PhysicalPad p;
    p.connected = t.connected;
    p.style = rr::platform::PadStyle::Touch;
    p.source = "OpenXR Touch";
    if (!t.connected) return p;
    if (t.touch & rr::xr::kTouchA) p.buttons |= Bit(PadButton::Cross);
    if (t.touch & rr::xr::kTouchB) p.buttons |= Bit(PadButton::Circle);
    if (t.touch & rr::xr::kTouchX) p.buttons |= Bit(PadButton::Square);
    if (t.touch & rr::xr::kTouchY) p.buttons |= Bit(PadButton::Triangle);
    if (t.touch & rr::xr::kTouchMenu) p.buttons |= Bit(PadButton::Start);
    if (t.touch & rr::xr::kTouchLeftStick) p.buttons |= Bit(PadButton::L3);
    if (t.touch & rr::xr::kTouchRightStick) p.buttons |= Bit(PadButton::R3);
    if (t.leftGrip > 0.5f) p.buttons |= Bit(PadButton::L1);
    if (t.rightGrip > 0.5f) p.buttons |= Bit(PadButton::R1);
    p.l2 = static_cast<uint8_t>(std::lround(std::clamp(t.leftTrigger, 0.0f, 1.0f) * 255.0f));
    p.r2 = static_cast<uint8_t>(std::lround(std::clamp(t.rightTrigger, 0.0f, 1.0f) * 255.0f));
    if (p.l2 >= rr::platform::kTriggerDown) p.buttons |= Bit(PadButton::L2);
    if (p.r2 >= rr::platform::kTriggerDown) p.buttons |= Bit(PadButton::R2);
    const auto byte = [](float v, bool invert) {
        const float c = std::clamp(v, -1.0f, 1.0f) * (invert ? -1.0f : 1.0f);
        return static_cast<uint8_t>(std::lround(127.5f + c * 127.5f));
    };
    p.lx = byte(t.stick[0][0], false);
    p.ly = byte(t.stick[0][1], true);
    p.rx = byte(t.stick[1][0], false);
    p.ry = byte(t.stick[1][1], true);
    rr::platform::AddStickDirections(p);
    constexpr float kThird = 1.0f / 3.0f; // the right stick as the d-pad positions
    if (t.stick[1][0] < -kThird) p.buttons |= Bit(PadButton::DLeft);
    if (t.stick[1][0] > kThird) p.buttons |= Bit(PadButton::DRight);
    if (t.stick[1][1] > kThird) p.buttons |= Bit(PadButton::DUp);
    if (t.stick[1][1] < -kThird) p.buttons |= Bit(PadButton::DDown);
    return p;
}

class VrHostImpl;

class VrPad final : public PadDevice {
public:
    VrPad(VrHostImpl& host, std::unique_ptr<PadDevice> desktop) : host_(host), desktop_(std::move(desktop)) {}
    rr::platform::GamepadState Poll(int nth) override;
    bool Vibrate(bool small, uint8_t large) override;
    void Triggers(bool racing) override {
        if (desktop_ && usedDesktop_) desktop_->Triggers(racing);
    }

private:
    VrHostImpl& host_;
    std::unique_ptr<PadDevice> desktop_; // Windows: a desktop controller works in VR too (and in the mock)
    bool usedDesktop_ = false;
};

// ---------------------------------------------------------------- the host
class VrHostImpl final : public VrHost {
public:
    explicit VrHostImpl(const VrHostConfig& config) : config_(config) {
#ifdef _WIN32
        desktop_ = CreateDesktopHost();
#endif
        // the settings: a scripted run the defaults (and the command line's --vr-* on top, main_*.cpp), an
        // interactive one the [vr] section of rrgame_settings.ini
        if (!config_.scripted) {
            bool rewrite = false;
            if (LoadVrSettings(SettingsPath(), VrPrefs(), &rewrite)) std::printf("vr settings: [vr] of %s\n", SettingsPath().c_str());
            else std::printf("vr settings: none in %s yet - the defaults\n", SettingsPath().c_str());
            if (rewrite && !SettingsMarkersOff()) { // a one-time migration ran: its marker written now, so it runs once
                const bool saved = SaveVrSettings(SettingsPath(), VrPrefs());
                std::printf("vr settings: [vr] of %s rewritten with its complete key list%s\n", SettingsPath().c_str(),
                            saved ? "" : " - FAILED");
            }
        }
        std::printf("%s\n", VrPrefs().Describe().c_str());
    }
    ~VrHostImpl() override {
        if (display_) display_->Vibrate(0.0f, 0.0f);
        display_.reset();
    }

    // ---- GameHost
    const char* Name() const override { return config_.mock ? "VR mock" : "VR (OpenXR)"; }
    void OpenWindow(int width, int height) override {
        if (desktop_) desktop_->OpenWindow(width, height); // the GL context (and on Windows the keyboard's window)
        if (display_) return;
        if (config_.mock) display_ = std::make_unique<MockDisplay>(config_, VrPrefs());
        else display_ = CreateXrDisplay(config_, VrPrefs());
        std::printf("vr: %s\n", display_->Describe().c_str());
        // single-pass stereo: every world program compiled from now on draws both eyes (render/multiview.h)
        rr::render::SetMultiviewPrograms(display_->Multiview());
        // the Touch bindings, loaded now so their line is in the log
        (void)rr::platform::VrBindings();
        if (config_.menuShotPage >= 0) menu_.Open(config_.menuShotPage);
    }
    bool Pump() override {
        if (desktop_ && !desktop_->Pump()) closed_ = true;
        if (config_.platformPump && !config_.platformPump(0)) closed_ = true;
        if (!display_) return !closed_;
        if (!display_->PollEvents()) closed_ = true;
        if (!closed_ && (!display_->Running() || !display_->Focused())) Block();
        // the Touch controllers, once a turn
        touch_ = rr::xr::XrPad{};
        if (display_->Running()) display_->PollPad(touch_);
        if (display_->TakeRecenterEvent()) recenter_.Request();
        return !closed_;
    }
    bool Closed() const override { return closed_ || (desktop_ && desktop_->Closed()); }
    const bool* Keys() const override { return desktop_ ? desktop_->Keys() : noKeys_; }
    void SetEscapeQuits(bool on) override {
        if (desktop_) desktop_->SetEscapeQuits(on);
    }
    void ClientRect(int& width, int& height) const override { width = 1280, height = 960; }
    void FrameSize(int& width, int& height) const override { width = 1280, height = 960; }
    void Present() override {} // the VR frame is submitted by EndFrame
    void SetVsync(bool) override {}
    void SetFullscreen(bool) override {}
    std::unique_ptr<PadDevice> OpenPad() override {
        return std::make_unique<VrPad>(*this, desktop_ ? desktop_->OpenPad() : nullptr);
    }
    VrHost* Vr() override { return this; }
    void LoadingTick() override {
        if (display_ && display_->Running()) IdleFrame();
    }

    // ---- VrHost
    VrDisplay& Display() override { return *display_; }
    const VrHostConfig& Config() const override { return config_; }
    bool BeginFrame() override {
        if (!display_ || !display_->Running()) return false;
        frameOpen_ = display_->BeginFrame();
        return frameOpen_;
    }
    bool ShouldRender() const override { return display_ && display_->ShouldRender(); }
    bool LocateStereo(const vr::Camera& camera, bool lookBack, float farZ, VrStereo& out) override {
        display_->KeepQuad(false); // a race frame: the quad only while the menu is drawn on it
        rr::xr::EyeView eyes[2];
        rr::xr::Pose head;
        if (!display_->LocateViews(eyes, head)) return false;
        if (recenter_.Pending()) {
            // the mock's head is turned by its configured yaw on purpose: its seat is latched facing ahead
            recenter_.Latch(config_.mock ? rr::xr::Pose{} : head);
            display_->RecenterQuad();
            std::printf("vr: recentred on the head (yaw %.1f deg)\n", double(recenter_.Yaw()) * 57.29577951308232);
        }
        vr::RigSettings rig = Rig();
        rig.lookBack = lookBack;
        out.anchor = vr::LevelledAnchor(camera, rig);
        out.nearZ = 0.05f * rig.unitsPerMetre;
        out.farZ = farZ;
        for (int e = 0; e < 2; ++e) {
            rr::xr::EyeView r = eyes[e];
            r.pose = recenter_.Apply(eyes[e].pose);
            out.eyes[e] = vr::PlaceEye(r, out.anchor, out.nearZ, farZ);
        }
        out.cullViewProj = vr::UnionViewProj(out.eyes, out.nearZ, farZ);
        return true;
    }
    rr::xr::FrameTarget BindEye(int eye) override { return display_->BindEye(eye); }
    void FinishEye(int eye) override {
        if (!shotPath_.empty()) display_->ReadEye(eye, shotEye_[eye & 1], shotW_[eye & 1], shotH_[eye & 1]);
        display_->FinishEye(eye);
    }
    bool Multiview() const override { return display_ && display_->Multiview(); }
    rr::xr::FrameTarget BindEyes() override { return display_->BindEyes(); }
    void FinishEyes() override {
        if (!shotPath_.empty())
            for (int e = 0; e < 2; ++e) display_->ReadEye(e, shotEye_[e], shotW_[e], shotH_[e]);
        display_->FinishEyes();
    }
    rr::xr::FrameTarget BindQuad() override { return display_->BindQuad(); }
    void FinishQuad() override {
        if (!shotPath_.empty()) display_->ReadQuad(shotQuad_, shotQuadW_, shotQuadH_);
        display_->FinishQuad();
    }
    void EndFrame() override {
        if (!frameOpen_) return;
        display_->EndFrame();
        frameOpen_ = false;
        if (!shotPath_.empty()) WriteShot();
    }
    bool TheatreFrame(const std::function<void(int, int)>& draw) override {
        if (!BeginFrame()) return false;
        if (ShouldRender()) {
            const rr::xr::FrameTarget t = BindQuad();
            glViewport(0, 0, t.width, t.height);
            draw(t.width, t.height);
            DrawMenuOver(t.width, t.height);
            FinishQuad();
            display_->KeepQuad(true);
        }
        EndFrame();
        return true;
    }
    void IdleFrame() override {
        if (!BeginFrame()) return;
        EndFrame();
    }
    void DrawMenuQuad() override {
        if (!MenuOpen() || !ShouldRender()) return;
        const rr::xr::FrameTarget t = BindQuad();
        glViewport(0, 0, t.width, t.height);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        DrawMenuOver(t.width, t.height);
        FinishQuad();
    }
    double DisplayPeriod() const override {
        const DisplayClock c = FrameClock(); // xrWaitFrame's period
        if (c.valid && c.period > 0) return static_cast<double>(c.period) * 1e-9;
        const double hz = display_ ? display_->RefreshRate() : 72.0;
        return hz > 1.0 ? 1.0 / hz : 1.0 / 72.0;
    }

    bool MenuHolds() override;
    bool MenuOpen() const override { return menu_.IsOpen(); }
    void RequestRecentre() override { recenter_.Request(); }
    DisplayClock FrameClock() const override { return display_ ? display_->Clock() : DisplayClock{}; }
    bool TakeTimingReset() override {
        const bool r = timingReset_ || (display_ && display_->TakeHeld()); // the mock's hold (vr_pacing.h)
        timingReset_ = false;
        return r;
    }
    void SetAudioMute(std::atomic<bool>* muted) override { mute_ = muted; }
    VrSettings& Settings() override { return VrPrefs(); }
    void SaveSettings() override {
        if (config_.scripted) return;
        if (!SaveVrSettings(SettingsPath(), VrPrefs())) std::fprintf(stderr, "vr settings: could not write %s\n", SettingsPath().c_str());
    }
    void ApplyGraphics(GraphicsSettings& gfx) override {
        const VrSettings& v = VrPrefs();
        gfx.renderScale = 100;
        gfx.renderHeight = 0;
        gfx.fullscreen = false;
        gfx.wide = true;
        gfx.vsync = false;
        gfx.fpsCap = 0;
        gfx.msaa = 1; // the eyes' own multisampling (the session's), not the desktop's offscreen target
        gfx.smoothTextures = v.smoothTextures;
        if (!config_.scripted) gfx.hdMedia = v.hdMedia; // a scripted run: its --hd-media only (hd_media.h)
        gfx.ps1Dither = false;
        gfx.ps1Colour = false;
        gfx.preciseVertices = true;
        gfx.affine = false; // perspective-correct, and no console subdividers
        gfx.ps1DrawOrder = false;
        gfx.maxDetail = v.maxDetail;
        gfx.drawDistance = v.drawDistance;
        gfx.profiler = false;
        gfx_ = &gfx;
    }
    vr::RigSettings Rig() const override {
        const VrSettings& v = VrPrefs();
        vr::RigSettings r;
        r.horizonLock = v.horizonMode == VrSettings::kHorizonOff ? 0.0f : static_cast<float>(v.horizonLock) / 100.0f;
        r.levelPitch = v.horizonMode == VrSettings::kHorizonRollPitch; // the roll only by default
        r.seatUp = static_cast<float>(v.seatHeightCm) / 100.0f;
        r.unitsPerMetre = static_cast<float>(v.worldScale) / 100.0f;
        return r;
    }
    void RequestShot(const std::string& path) override {
        shotPath_ = path;
        shotEye_[0].clear();
        shotEye_[1].clear();
        shotQuad_.clear();
        shotDone_ = false;
    }
    bool ShotDone() const override { return shotDone_; }

    // ---- the hands (vr_handlebars.h)
    const rr::xr::XrPad& TouchState() const override { return touch_; }
    bool LocateHands(rr::xr::HandPose hands[2]) override {
        if (!display_ || !display_->LocateHands(hands)) return false;
        for (int h = 0; h < 2; ++h) // recentred as the eyes are (LocateStereo)
            for (float* p : {hands[h].grip, hands[h].aim}) {
                rr::xr::Pose local;
                std::memcpy(local.position, p, 3 * sizeof(float));
                std::memcpy(local.orientation, p + 3, 4 * sizeof(float));
                const rr::xr::Pose r = recenter_.Apply(local);
                std::memcpy(p, r.position, 3 * sizeof(float));
                std::memcpy(p + 3, r.orientation, 4 * sizeof(float));
            }
        return true;
    }
    void SetHandHaptics(float left, float right) override {
        const float k = static_cast<float>(VrPrefs().vibration) / 100.0f;
        handAmp_[0] = left * k;
        handAmp_[1] = right * k;
        ApplyHaptics();
    }
    void SetHapticsRacing(bool racing) override {
        raceSeen_ = true;
        if (racing == racing_) return;
        racing_ = racing;
        if (!racing) gameAmp_[0] = gameAmp_[1] = handAmp_[0] = handAmp_[1] = 0.0f; // nothing stale when it comes back
        ApplyHaptics(); // stopped at once
    }

    // for VrPad
    const rr::xr::XrPad& Touch() const { return touch_; }
    bool InputHeld() const { return menu_.IsOpen() || swallow_; }
    void Haptics(bool small, uint8_t large) {
        if (!display_) return;
        const float k = static_cast<float>(VrPrefs().vibration) / 100.0f;
        const float big = static_cast<float>(large) / 255.0f;
        gameAmp_[0] = big * k;
        gameAmp_[1] = std::max(big, small ? 0.35f : 0.0f) * k;
        ApplyHaptics();
    }
    // the haptics are held (nothing sent, the actuators stopped) while the VR menu is open or its buttons
    // are not yet let go, the race is not racing (the pause menu, the results, the front end) or the session is not
    // focused. Otherwise the hands' haptics (sent every drawn frame, also while the menu holds the race) would re-send
    // the last game rumble level and the controllers would buzz under the menu.
    bool HapticsHeld() const { return menu_.IsOpen() || swallow_ || !racing_ || !display_ || !display_->Focused(); }
    void ApplyHaptics() { // the game's rumble and the hands' own (vr_handlebars.h), the stronger per hand
        if (!display_) return;
        const bool held = HapticsHeld();
        float left = std::max(gameAmp_[0], handAmp_[0]), right = std::max(gameAmp_[1], handAmp_[1]);
        if (held && !HapticsHoldOff()) left = right = 0.0f;
        MockDisplay* mock = config_.mock ? static_cast<MockDisplay*>(display_.get()) : nullptr;
        const uint64_t applies = mock ? mock->HapticApplies() : 0, stops = mock ? mock->HapticStops() : 0;
        display_->Vibrate(left, right);
        if (mock == nullptr || !raceSeen_) return;
        ComfortCounters& c = Comfort();
        const uint64_t da = mock->HapticApplies() - applies, ds = mock->HapticStops() - stops;
        (held ? c.appliesHeld : c.appliesRacing) += da;
        (held ? c.stopsHeld : c.stopsRacing) += ds;
        if (menu_.IsOpen()) c.appliesMenu += da;
        else if (held && !racing_) c.appliesPause += da;
    }
    bool HasDesktop() const { return desktop_ != nullptr; }

private:
    void Block();
    void DrawMenuOver(int w, int h) {
        if (!menu_.IsOpen()) return;
        if (!text_.Ready()) text_.Init();
        menu_.Draw(text_, w, h, status_);
    }
    void WriteShot();

    VrHostConfig config_;
    std::unique_ptr<GameHost> desktop_;
    std::unique_ptr<VrDisplay> display_;
    bool closed_ = false, frameOpen_ = false, timingReset_ = false, swallow_ = false;
    bool racing_ = false, raceSeen_ = false; // the race's SetHapticsRacing
    long padTurn_ = 0;                       // the mock's scripted controllers (--vr-mock-pad): the loop's turn
    bool noKeys_[256] = {};
    rr::xr::XrPad touch_;
    float gameAmp_[2] = {0, 0}, handAmp_[2] = {0, 0}; // the rumble's and the hands' haptics
    vr::Recenter recenter_;
    VrMenu menu_;
    rr::render::TextOverlay text_;
    std::string status_;
    std::atomic<bool>* mute_ = nullptr;
    GraphicsSettings* gfx_ = nullptr;
    // the menu's input edges
    bool chordWas_ = false, f10Was_ = false;
    uint32_t navWas_ = 0;
    double repeatAt_ = 0;
    int repeatDir_ = 0;
    // shots
    std::string shotPath_;
    std::vector<uint8_t> shotEye_[2], shotQuad_;
    int shotW_[2] = {0, 0}, shotH_[2] = {0, 0}, shotQuadW_ = 0, shotQuadH_ = 0;
    bool shotDone_ = false;
};

rr::platform::GamepadState VrPad::Poll(int nth) {
    const rr::xr::XrPad& t = host_.Touch();
    usedDesktop_ = false;
    if (nth == 0 && t.connected) {
        rr::platform::PhysicalPad p = TouchToPhysical(t);
        if (VrPrefs().bars.steering == BarsSettings::kHandlebars) { // vr_handlebars.h: the grips hold the bars, the
            using rr::platform::Bit;                                 // triggers are the analogue throttle / brake
            using rr::platform::PadButton;
            p.buttons &= ~(Bit(PadButton::L1) | Bit(PadButton::R1) | Bit(PadButton::L2) | Bit(PadButton::R2));
            p.l2 = p.r2 = 0;
        }
        rr::platform::GamepadState s = rr::platform::StateFromPhysical(p, rr::platform::VrBindings());
        // the menus by the Quest's convention: A confirms (Cross), B goes back (Triangle), X Square, Y Circle
        s.cross = (t.touch & rr::xr::kTouchA) != 0;
        s.triangle = (t.touch & rr::xr::kTouchB) != 0;
        s.square = (t.touch & rr::xr::kTouchX) != 0;
        s.circle = (t.touch & rr::xr::kTouchY) != 0;
        s.hasMotors = true;
        if (host_.InputHeld()) { // the VR menu has the controllers: the game sees a released pad
            rr::platform::GamepadState idle;
            idle.connected = true;
            idle.source = s.source;
            idle.hasMotors = true;
            idle.raw = p;
            return idle;
        }
        return s;
    }
    if (desktop_) { // Windows: the n-th desktop controller (the first when no Touch controller is on)
        usedDesktop_ = true;
        return desktop_->Poll(t.connected ? nth - 1 : nth);
    }
    return {};
}

bool VrPad::Vibrate(bool small, uint8_t large) {
    if (usedDesktop_ && desktop_) return desktop_->Vibrate(small, large);
    host_.Haptics(small, large);
    return true;
}

void VrHostImpl::Block() {
    // The session is not running or not focused (the headset taken off, the system menu, the Guardian): the game is
    // held - no simulation, the sound muted - and the compositor still gets frames while the session runs.
    if (mute_) mute_->store(true, std::memory_order_relaxed);
    ApplyHaptics(); // not focused - the actuators stopped now (the session sends nothing unfocused either)
    std::printf("vr: session %s - the game is held\n", display_->StateName());
    long waited = 0;
    while (!closed_ && (!display_->Running() || !display_->Focused())) {
        if (desktop_ && !desktop_->Pump()) closed_ = true;
        if (config_.platformPump && !config_.platformPump(display_->Running() ? 0 : 100)) closed_ = true;
        if (!display_->PollEvents()) closed_ = true;
        if (closed_) break;
        if (display_->Running()) {
            if (BeginFrame()) EndFrame(); // an empty frame keeps the session's frame loop turning
        } else {
            if (!config_.platformPump) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (++waited % 1000 == 0) std::printf("vr: still waiting for the session (%s)\n", display_->StateName());
        }
    }
    if (mute_) mute_->store(false, std::memory_order_relaxed);
    timingReset_ = true;
    if (!closed_) {
        std::printf("vr: session %s - the game runs\n", display_->StateName());
        recenter_.Request();
    }
}

bool VrHostImpl::MenuHolds() {
    if (config_.mock && MockPadScripted()) MockPad(padTurn_++, touch_); // --vr-mock-pad
    const rr::xr::XrPad& t = touch_;
    const bool* keys = Keys();
    // the chord: both stick clicks, or both grips + Menu; F10 on a keyboard
    const bool sticks = (t.touch & rr::xr::kTouchLeftStick) && (t.touch & rr::xr::kTouchRightStick);
    const bool grips = t.leftGrip > 0.5f && t.rightGrip > 0.5f && (t.touch & rr::xr::kTouchMenu);
    const bool chord = t.connected && (sticks || grips);
    const bool f10 = keys[VK_F10];
    const bool chordEdge = (chord && !chordWas_) || (f10 && !f10Was_);
    chordWas_ = chord;
    f10Was_ = f10;
    if (!menu_.IsOpen()) {
        if (chordEdge && config_.menuShotPage < 0) {
            menu_.Open(0);
            swallow_ = true;
            navWas_ = ~0u; // nothing counts as pressed until it is let go
            ++Comfort().menuOpened;
            ApplyHaptics(); // the rumble stops the moment the menu opens
        }
    }
    if (menu_.IsOpen()) {
        if (config_.menuShotPage >= 0) { // the development shot: drawn open, no input, the game runs on
            menu_.Update(VrMenuInput{}, display_ ? display_->RefreshRates() : std::vector<float>{});
            return false;
        }
        // navigation: the left (or right) stick up / down chooses the row, the TRIGGERS change its value (left
        // decreases, right increases - GT2's rule; the stick's left / right do not change values, a drifting
        // stick would do it by itself), A / B / Menu; the keyboard's arrows, Enter, Esc / F10. Every page (settings,
        // controls, combat, cheats and the pages added later) goes through here.
        const float sy = std::fabs(t.stick[0][1]) > std::fabs(t.stick[1][1]) ? t.stick[0][1] : t.stick[1][1];
        const float sx = std::fabs(t.stick[0][0]) > std::fabs(t.stick[1][0]) ? t.stick[0][0] : t.stick[1][0];
        const bool stickValues = MenuStickChangesValues(); // RRJB_VR_MENU_STICK=values (the control)
        uint32_t nav = 0;
        enum { kUp = 1, kDown = 2, kLeft = 4, kRight = 8, kOk = 16, kBack = 32, kClose = 64 };
        if (sy > 0.6f || keys[VK_UP]) nav |= kUp;
        if (sy < -0.6f || keys[VK_DOWN]) nav |= kDown;
        if ((stickValues && sx < -0.6f) || t.leftTrigger > 0.6f || keys[VK_LEFT]) nav |= kLeft;
        if ((stickValues && sx > 0.6f) || t.rightTrigger > 0.6f || keys[VK_RIGHT]) nav |= kRight;
        if ((t.touch & rr::xr::kTouchA) || keys[VK_RETURN] || keys[VK_SPACE]) nav |= kOk;
        if ((t.touch & rr::xr::kTouchB) || keys[VK_ESCAPE] || keys[VK_BACK]) nav |= kBack;
        if (((t.touch & rr::xr::kTouchMenu) && !grips) || (f10 && chordEdge)) nav |= kClose;
        if (navWas_ == ~0u) navWas_ = nav; // the chord's own buttons are not the menu's input
        const uint32_t edge = nav & ~navWas_;
        VrMenuInput in;
        in.up = (edge & kUp) != 0;
        in.down = (edge & kDown) != 0;
        in.left = (edge & kLeft) != 0;
        in.right = (edge & kRight) != 0;
        { // held left / right repeat (the long lists)
            const double now = std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
            const int held = (nav & kLeft) ? -1 : (nav & kRight) ? 1 : 0;
            if (in.left || in.right) {
                repeatDir_ = held;
                repeatAt_ = now + 0.45;
            } else if (held != 0 && held == repeatDir_ && now >= repeatAt_) {
                (held < 0 ? in.left : in.right) = true;
                repeatAt_ = now + 0.15;
            } else if (held == 0) {
                repeatDir_ = 0;
            }
        }
        in.confirm = (edge & kOk) != 0;
        in.back = (edge & kBack) != 0;
        in.close = (edge & kClose) != 0;
        const rr::platform::PhysicalPad p = TouchToPhysical(t);
        in.pad = &p;
        navWas_ = nav;
        const std::string rowBefore = menu_.SelectedRowText();
        const VrMenuActions a = menu_.Update(in, display_ ? display_->RefreshRates() : std::vector<float>{});
        { // which input changed a value (the menu rule's evidence)
            ComfortCounters& c = Comfort();
            if (in.up || in.down) ++c.menuRows;
            if (in.confirm) ++c.menuConfirm;
            const std::string rowAfter = menu_.SelectedRowText();
            if ((in.left || in.right) && rowAfter != rowBefore && !rowBefore.empty()) {
                const bool trigger = t.leftTrigger > 0.6f || t.rightTrigger > 0.6f;
                const bool stick = stickValues && std::fabs(sx) > 0.6f;
                const char* by = trigger ? "a trigger" : stick ? "the stick" : "the keys";
                ++(trigger ? c.menuTrigger : stick ? c.menuStick : c.menuKeys);
                std::printf("vr menu: %s -> %s (by %s)\n", rowBefore.c_str(), rowAfter.c_str(), by);
            }
        }
        VrSettings& v = VrPrefs();
        if (a.recentre) recenter_.Request();
        if (a.refreshChanged && display_) {
            status_ = display_->SetRefreshRate(static_cast<float>(v.refreshHz)) ? "" : "The runtime refused that refresh rate";
        }
        if (a.msaaChanged && display_) display_->SetEyeSamples(v.msaa);
        if (a.foveationChanged && display_) display_->SetFoveation(v.foveation);
        if (a.graphicsChanged && gfx_ != nullptr) ApplyGraphics(*gfx_);
        if (a.settingsChanged || a.refreshChanged || a.msaaChanged || a.foveationChanged || a.graphicsChanged) SaveSettings();
        if (a.quit) {
            std::printf("vr: Quit game from the VR menu\n");
            closed_ = true;
            if (display_) display_->RequestExit();
        }
    }
    if (!menu_.IsOpen() && swallow_) { // closed: held until the buttons that closed it are let go
        const bool anything = (t.touch != 0) || t.leftTrigger > 0.25f || t.rightTrigger > 0.25f || keys[VK_ESCAPE] ||
                              keys[VK_RETURN] || keys[VK_SPACE] || keys[VK_BACK] || keys[VK_F10];
        if (!anything) swallow_ = false;
    }
    const bool hold = menu_.IsOpen() || swallow_;
    if (mute_) mute_->store(hold, std::memory_order_relaxed);
    if (raceSeen_) { // the haptics' accounting per loop turn
        ComfortCounters& c = Comfort();
        if (HapticsHeld()) {
            ++c.hapticHeldTurns;
            if (menu_.IsOpen()) ++c.menuTurns;
            else if (!racing_) ++c.pauseTurns;
        } else {
            ++c.hapticRacingTurns;
        }
        if (hold) ApplyHaptics(); // held: the actuators kept stopped whoever else calls
    }
    return hold;
}

void VrHostImpl::WriteShot() {
    // the pair side by side, the quad (when it was drawn this frame) under it, centred; bottom row first as GL gives
    // them, flipped by SaveShot
    const bool eyes = !shotEye_[0].empty() && !shotEye_[1].empty() && shotH_[0] == shotH_[1];
    const bool quad = !shotQuad_.empty();
    if (!eyes && !quad) return; // nothing drawn this frame yet (the next frame is tried)
    const int ew = eyes ? shotW_[0] + shotW_[1] : 0, eh = eyes ? shotH_[0] : 0;
    int qw = quad ? shotQuadW_ : 0, qh = quad ? shotQuadH_ : 0;
    const int width = std::max(ew, qw), height = eh + qh;
    std::vector<uint8_t> out(size_t(width) * size_t(height) * 4, 0);
    for (size_t i = 3; i < out.size(); i += 4) out[i] = 255;
    const auto blit = [&](const std::vector<uint8_t>& src, int sw, int sh, int dx, int dy) { // dy: from the bottom
        for (int y = 0; y < sh; ++y)
            std::memcpy(out.data() + (size_t(dy + y) * size_t(width) + size_t(dx)) * 4, src.data() + size_t(y) * size_t(sw) * 4,
                        size_t(sw) * 4);
    };
    if (eyes) {
        blit(shotEye_[0], shotW_[0], shotH_[0], (width - ew) / 2, qh);
        blit(shotEye_[1], shotW_[1], shotH_[1], (width - ew) / 2 + shotW_[0], qh);
    }
    if (quad) blit(shotQuad_, qw, qh, (width - qw) / 2, 0);
    // opaque, as the compositor shows the layers (a projection layer's alpha is not used; the shadow's reverse
    // subtraction leaves alpha 0 under it)
    for (size_t i = 3; i < out.size(); i += 4) out[i] = 255;
    rr::render::SaveShot(shotPath_, width, height, out);
    std::printf("vr shot: %s - %s%s\n", shotPath_.c_str(), eyes ? "the left and right eye side by side" : "",
                quad ? (eyes ? ", the theatre quad below" : "the theatre quad") : "");
    shotPath_.clear();
    shotDone_ = true;
}

} // namespace

std::unique_ptr<VrHost> CreateVrHost(const VrHostConfig& config) { return std::make_unique<VrHostImpl>(config); }

} // namespace rrgame
