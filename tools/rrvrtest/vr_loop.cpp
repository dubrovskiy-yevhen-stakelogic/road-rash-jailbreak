#include "vr_loop.h"

#include "platform/png.h"
#include "render/gl_api.h"
#include "render/render_target.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

namespace rrvr {
namespace {

// The bound framebuffer (the eye's own RGBA8 target) as a PNG, top row first.
void Shot(const std::string& path, int w, int h) {
    std::vector<uint8_t> pixels(size_t(w) * size_t(h) * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    std::vector<uint8_t> flipped(pixels.size());
    const size_t row = size_t(w) * 4;
    for (int y = 0; y < h; ++y)
        std::copy(pixels.begin() + std::ptrdiff_t(size_t(y) * row), pixels.begin() + std::ptrdiff_t(size_t(y + 1) * row),
                  flipped.begin() + std::ptrdiff_t(size_t(h - 1 - y) * row));
    rr::WritePng(path, w, h, flipped);
    std::printf("rrvrtest: wrote %s (%dx%d)\n", path.c_str(), w, h);
}

// The synthetic head of MockPair / MockTiming: seated at the LOCAL origin, eyes 64 mm apart, a Quest-3-like
// asymmetric field (the tangents are OURS, round numbers near what the Quest 3 reports).
void MockEyes(rr::xr::EyeView eyes[2]) {
    eyes[0] = eyes[1] = rr::xr::EyeView{};
    eyes[0].pose.position[0] = -0.032f;
    eyes[1].pose.position[0] = 0.032f;
    eyes[0].fov = {-0.9f, 0.75f, 0.8f, -0.85f};
    eyes[1].fov = {-0.75f, 0.9f, 0.8f, -0.85f};
}

void DrawMockEyes(ProofScene& scene, int eyeW, int eyeH, const LoopOptions& options) {
    rr::xr::EyeView eyes[2];
    MockEyes(eyes);
    const rr::xr::WorldAnchor anchor = scene.Anchor(options.unitsPerMetre, options.eyeHeight);
    for (int e = 0; e < 2; ++e) {
        const rr::xr::WorldEye view = rr::xr::PlaceInWorld(eyes[e].pose, anchor);
        const rr::render::Mat4 proj = rr::xr::ProjectionFromFov(eyes[e].fov, 0.05f * options.unitsPerMetre, 20000.0f);
        glEnable(GL_SCISSOR_TEST); // DrawView clears: keep each eye's clear inside its half
        glScissor(e * eyeW, 0, eyeW, eyeH);
        scene.DrawView(proj, view, e * eyeW, 0, eyeW, eyeH, options.ps1Look);
        glDisable(GL_SCISSOR_TEST);
    }
}

} // namespace

bool MockPair(ProofScene& scene, int w, int h, const std::string& png, const LoopOptions& options) {
    rr::render::RenderTarget target;
    if (!target.Ensure(w, h, 1)) return false;
    target.Bind();
    DrawMockEyes(scene, w / 2, h, options);
    glFinish();
    Shot(png, w, h);
    rr::render::BindFramebuffer(0);
    return true;
}

double MockTiming(ProofScene& scene, int eyeW, int eyeH, int frames, const LoopOptions& options) {
    rr::render::RenderTarget target;
    if (!target.Ensure(2 * eyeW, eyeH, 1) || frames <= 0) return -1.0;
    target.Bind();
    DrawMockEyes(scene, eyeW, eyeH, options); // warm-up: first-use uploads and shader variants
    glFinish();
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < frames; ++i) DrawMockEyes(scene, eyeW, eyeH, options);
    glFinish();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / frames;
    rr::render::BindFramebuffer(0);
    std::printf("rrvrtest: mock timing: %d stereo frames of 2 x %dx%d, %.2f ms each (CPU + GPU, glFinish-bounded)\n",
                frames, eyeW, eyeH, ms);
    return ms;
}

int RunLoop(ProofScene& scene, rr::xr::GlSession& session, const LoopOptions& options) {
    uint16_t lastButtons = 0;
    bool lastConnected = false;
    long long rendered = 0;
    bool shotDone = false;
    auto lastMove = std::chrono::steady_clock::now();
    while (!options.keepGoing || options.keepGoing()) {
        if (!session.PollEvents()) break;
        if (!session.Running()) {
            if (options.idle) options.idle();
            else std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (!session.BeginFrame()) continue;

        rr::xr::XrPad pad;
        session.PollPad(pad);
        if (pad.connected != lastConnected || pad.buttons != lastButtons) {
            std::printf("pad: %s buttons 0x%04X  lx %3u ly %3u rx %3u ry %3u  triggers %.2f %.2f  grips %.2f %.2f\n",
                        pad.source, unsigned(pad.buttons), unsigned(pad.lx), unsigned(pad.ly), unsigned(pad.rx),
                        unsigned(pad.ry), double(pad.leftTrigger), double(pad.rightTrigger), double(pad.leftGrip),
                        double(pad.rightGrip));
            if ((pad.buttons & rr::xr::kPadCross) && !(lastButtons & rr::xr::kPadCross)) session.RecenterQuad();
            lastButtons = pad.buttons;
            lastConnected = pad.connected;
        }
        // the DualShock's motors on the Touch haptics: the triggers' travel, a test of the haptic path
        session.Vibrate(pad.leftTrigger * 0.6f, pad.rightTrigger * 0.6f);
        { // the left stick moves the viewer along the route (6 m/s at full travel)
            const auto now = std::chrono::steady_clock::now();
            const float dt = std::chrono::duration<float>(now - lastMove).count();
            lastMove = now;
            const float stick = (128.0f - float(pad.ly)) / 127.0f;
            if (std::fabs(stick) > 0.2f) scene.Place(scene.Distance() + stick * 6.0f * dt, false);
        }

        if (session.ShouldRender()) {
            rr::xr::EyeView eyes[2];
            rr::xr::Pose head;
            if (session.LocateViews(eyes, head)) {
                const rr::xr::WorldAnchor anchor = scene.Anchor(options.unitsPerMetre, options.eyeHeight);
                const bool shoot = !options.shotDir.empty() && !shotDone && rendered >= options.shotFrame;
                for (int e = 0; e < 2; ++e) {
                    const rr::xr::WorldEye view = rr::xr::PlaceInWorld(eyes[e].pose, anchor);
                    // near 0.05 m: a head can come that close to the parked machine; far as the desktop's 20000 units
                    const rr::render::Mat4 proj =
                        rr::xr::ProjectionFromFov(eyes[e].fov, 0.05f * options.unitsPerMetre, 20000.0f);
                    const rr::xr::FrameTarget t = session.BindEye(e);
                    scene.DrawView(proj, view, 0, 0, t.width, t.height, options.ps1Look);
                    if (shoot) Shot(options.shotDir + (e == 0 ? "/xr_eye0.png" : "/xr_eye1.png"), t.width, t.height);
                    session.FinishEye(e);
                }
                if (options.quad) {
                    const rr::xr::FrameTarget t = session.BindQuad();
                    scene.DrawFlat(0, 0, t.width, t.height, true);
                    if (shoot) Shot(options.shotDir + "/xr_quad.png", t.width, t.height);
                    session.FinishQuad();
                }
                if (shoot) {
                    shotDone = true;
                    std::printf("rrvrtest: head at (%.3f, %.3f, %.3f) m; eye 0 fov L %.1f R %.1f U %.1f D %.1f deg\n",
                                double(head.position[0]), double(head.position[1]), double(head.position[2]),
                                double(eyes[0].fov.left) * 57.2958, double(eyes[0].fov.right) * 57.2958,
                                double(eyes[0].fov.up) * 57.2958, double(eyes[0].fov.down) * 57.2958);
                }
                ++rendered;
            }
        }
        session.EndFrame();
        if (options.frames > 0 && session.FrameIndex() >= options.frames) {
            std::printf("rrvrtest: %lld frames submitted (%lld rendered in stereo), stopping\n", session.FrameIndex(), rendered);
            break;
        }
    }
    session.Vibrate(0, 0);
    // A clean end: xrRequestExitSession, then keep the frame loop turning (empty frames) until the runtime has taken
    // the session through STOPPING (xrEndSession) to IDLE / EXITING - at most 3 s.
    if (session.Running()) {
        session.RequestExit();
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < until && session.PollEvents() && session.Running()) {
            if (session.BeginFrame()) session.EndFrame();
        }
        std::printf("rrvrtest: session %s after the exit request\n", session.StateName());
    }
    return 0;
}

} // namespace rrvr
