// rrvrtest (Quest): the standalone proof of the OpenXR layer on OpenGL ES.
//
// A NativeActivity (android/app/src/main/AndroidManifest.xml, lib_name rrvrtest). Everything runs on android_main's
// thread: the activity's looper is serviced between OpenXR frames. The player's own disc image is read from the app's
// external files directory (/sdcard/Android/data/com.rrjb.vr/files/, pushed there with adb - see
// scripts/install-quest.ps1); an optional rrvrtest.txt next to it sets the run (key=value lines):
//   race=1 20   distance=20   frames=0 (0: until quit)   scale=1.0   refresh=0   ps1=0   quad=1   shot=240 (-1: none)
// stdout and stderr go to logcat under the tag RRJB.VR (every line the renderer and the XR layer print).
#include <android/log.h>
#include <android_native_app_glue.h>
#include <jni.h>
#include <unistd.h>

#include "platform/app_paths.h"
#include "platform/xr/egl_context_android.h"
#include "platform/xr/xr_session_gl.h"
#include "render/gl_api.h"
#include "rrvfs/disc_image.h"
#include "vr_loop.h"
#include "vr_scene.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

constexpr const char* kTag = "RRJB.VR";

// stdout / stderr -> logcat, line by line, on a thread of its own.
void RedirectStdioToLogcat() {
    static int fds[2];
    if (pipe(fds) != 0) return;
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    std::thread([] {
        char buf[1024];
        std::string line;
        for (;;) {
            const ssize_t n = read(fds[0], buf, sizeof(buf));
            if (n <= 0) break;
            for (ssize_t i = 0; i < n; ++i) {
                if (buf[i] == '\n') {
                    __android_log_write(ANDROID_LOG_INFO, kTag, line.c_str());
                    line.clear();
                } else {
                    line += buf[i];
                }
            }
        }
    }).detach();
}

struct Host {
    bool resumed = false;
};

void OnCommand(android_app* app, int32_t command) {
    auto* host = static_cast<Host*>(app->userData);
    switch (command) {
    case APP_CMD_RESUME: host->resumed = true; std::printf("android: resumed\n"); break;
    case APP_CMD_PAUSE: host->resumed = false; std::printf("android: paused\n"); break;
    case APP_CMD_DESTROY: std::printf("android: destroy\n"); break;
    default: break;
    }
}

// Services the activity's queue; `timeoutMs` 0 returns at once. False once the activity is being destroyed.
bool Pump(android_app* app, int timeoutMs) {
    for (;;) {
        int events = 0;
        android_poll_source* source = nullptr;
        const int r = ALooper_pollOnce(timeoutMs, nullptr, &events, reinterpret_cast<void**>(&source));
        if (r < 0) break; // timeout / wake: nothing (more) queued
        if (source) source->process(app, source);
        if (app->destroyRequested) return false;
        timeoutMs = 0;
    }
    return !app->destroyRequested;
}

struct Config {
    int set = 1, race = 20;
    float distance = 20.0f, scale = 1.0f, refresh = 0.0f;
    long long frames = 0, shot = 240;
    int mock = 0;   // > 0: before the session, the offscreen mock pair (mock_eyes.png) and this many timed stereo frames
    bool xr = true; // 0: stop after the mock (no OpenXR session)
    bool ps1 = false, quad = true;
};

Config ReadConfig(const std::filesystem::path& file) {
    Config c;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        std::istringstream value(line.substr(eq + 1));
        if (key == "race") value >> c.set >> c.race;
        else if (key == "distance") value >> c.distance;
        else if (key == "frames") value >> c.frames;
        else if (key == "scale") value >> c.scale;
        else if (key == "refresh") value >> c.refresh;
        else if (key == "shot") value >> c.shot;
        else if (key == "ps1") { int v = 0; value >> v; c.ps1 = v != 0; }
        else if (key == "quad") { int v = 1; value >> v; c.quad = v != 0; }
        else if (key == "mock") value >> c.mock;
        else if (key == "xr") { int v = 1; value >> v; c.xr = v != 0; }
    }
    return c;
}

} // namespace

void android_main(android_app* app) {
    RedirectStdioToLogcat();
    Host host;
    app->userData = &host;
    app->onAppCmd = OnCommand;
    JNIEnv* env = nullptr;
    app->activity->vm->AttachCurrentThread(&env, nullptr);
    rr::platform::SetAppDirs(app->activity->externalDataPath ? app->activity->externalDataPath : "",
                             app->activity->internalDataPath ? app->activity->internalDataPath : "");
    // the edge rule's geometry stage has no console grid to decide in an eye image (edge_rule.h); off in VR
    setenv("RRJB_EDGE", "gl", 0);
    std::printf("rrvrtest (Quest): data %s\n", rr::platform::DataRoot().string().c_str());
    int code = 0;
    try {
        const Config config = ReadConfig(rr::platform::DataRoot() / "rrvrtest.txt");
        const std::filesystem::path discPath = rr::platform::FindDiscImage();
        if (discPath.empty())
            throw std::runtime_error("no disc image in " + rr::platform::DataRoot().string() +
                                     " - push the player's own Road Rash: Jailbreak (USA) .bin there as disc.bin");
        std::printf("rrvrtest: disc %s\n", discPath.string().c_str());
        rr::xr::EglContext egl;
        rr::render::LoadGl();
        rr::DiscImage disc(discPath.string());
        rrvr::ProofScene scene(disc, config.set, config.race, config.distance);
        if (config.mock > 0) {
            rrvr::LoopOptions mockOptions;
            mockOptions.ps1Look = config.ps1;
            if (!rrvr::MockPair(scene, 1280, 720, (rr::platform::DataRoot() / "mock_eyes.png").string(), mockOptions))
                throw std::runtime_error("no offscreen render target");
            rrvr::MockTiming(scene, 1680, 1760, config.mock, mockOptions);
        }
        if (!config.xr) throw std::runtime_error("xr=0: no OpenXR session requested (mock only)");
        rr::xr::SessionOptions options;
        options.appName = "rrvrtest";
        options.binding.eglDisplay = egl.Display();
        options.binding.eglConfig = egl.Config();
        options.binding.eglContext = egl.Context();
        options.androidVm = app->activity->vm;
        options.androidActivity = app->activity->clazz;
        options.eyeScale = config.scale;
        options.refreshHz = config.refresh;
        options.quadYawOffset = 0.6f;
        rr::xr::GlSession session(options);
        rrvr::LoopOptions loop;
        loop.frames = config.frames > 0 ? config.frames : -1;
        loop.ps1Look = config.ps1;
        loop.quad = config.quad;
        if (config.shot >= 0) {
            loop.shotDir = rr::platform::DataRoot().string();
            loop.shotFrame = config.shot;
        }
        loop.keepGoing = [app] { return Pump(app, 0); };
        loop.idle = [app] { Pump(app, 10); };
        code = rrvr::RunLoop(scene, session, loop);
    } catch (const std::exception& e) {
        std::printf("rrvrtest: FAILED: %s\n", e.what());
        __android_log_print(ANDROID_LOG_ERROR, kTag, "FAILED: %s", e.what());
        code = 1;
    }
    std::printf("rrvrtest: exit %d\n", code);
    app->activity->vm->DetachCurrentThread();
    ANativeActivity_finish(app->activity);
    while (Pump(app, 100)) {
    }
}
