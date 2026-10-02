// rrgame on the Quest (docs\QUEST.md): the NativeActivity's android_main runs the
// real game - the front end, the career, the races, the saves - on the VR host (game_host_vr.h) with OpenXR over
// OpenGL ES 3.2 (XR_KHR_opengl_es_enable) and the sound through AAudio.
//
//   * The disc: the player's own image in the app's external files folder (/sdcard/Android/data/com.rrjb.vr/files,
//     `disc.bin` first - scripts\install-quest-player.ps1 copies it there; platform/app_paths.h FindDiscImage).
//   * The saves: files/saves in the app's INTERNAL storage - the memory card rrjb_card.mcr, rrgame_settings.ini (the
//     [vr] settings) and controls.ini ([vr_controls]); scripts\transfer-saves.ps1 reaches the card through the save provider.
//   * DEVELOPMENT: files/rrgame_args.txt, when present, holds more command-line switches (whitespace-separated), e.g.
//     "--race 1 20" (straight into a race), "--vr-mock --race 1 20 --frames 900 --shot mock.png" (the offscreen VR
//     frames without a session: the mock's timing on the Quest's GPU). Relative paths are the external files folder.
// Everything the game prints goes to logcat under the tag RRJB.VR.
#include <android/log.h>
#include <android_native_app_glue.h>
#include <jni.h>
#include <unistd.h>

#include "game/shell/front_end.h"
#include "game_host.h"
#include "game_host_vr.h"
#include "platform/app_paths.h"
#include "platform/xr/egl_context_android.h"
#include "render/gl_api.h"
#include "vr_settings.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

int RaceMainLoop(int argc, char** argv); // main.cpp

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

struct ActivityState {
    bool resumed = false;
};

void OnCommand(android_app* app, int32_t command) {
    auto* state = static_cast<ActivityState*>(app->userData);
    switch (command) {
    case APP_CMD_RESUME: state->resumed = true; std::printf("android: resumed\n"); break;
    case APP_CMD_PAUSE: state->resumed = false; std::printf("android: paused\n"); break;
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

std::vector<std::string> ReadArgs(const std::filesystem::path& file) {
    std::vector<std::string> out;
    std::ifstream in(file);
    std::string word;
    while (in >> word) out.push_back(word);
    return out;
}

} // namespace

void android_main(android_app* app) {
    RedirectStdioToLogcat();
    ActivityState state;
    app->userData = &state;
    app->onAppCmd = OnCommand;
    JNIEnv* env = nullptr;
    app->activity->vm->AttachCurrentThread(&env, nullptr);
    rr::platform::SetAppDirs(app->activity->externalDataPath ? app->activity->externalDataPath : "",
                             app->activity->internalDataPath ? app->activity->internalDataPath : "");
    // relative paths (a shot, a log) land in the external files folder, reachable over adb
    if (chdir(rr::platform::DataRoot().string().c_str()) != 0) std::printf("rrgame: cannot enter %s\n", rr::platform::DataRoot().string().c_str());
    // the edge rule's geometry stage has no console grid to decide in an eye image (edge_rule.h): GL's own coverage
    setenv("RRJB_EDGE", "gl", 0);
    std::printf("rrgame (Quest): data %s, saves %s\n", rr::platform::DataRoot().string().c_str(),
                rr::platform::SavesDir().string().c_str());
    int code = 0;
    std::unique_ptr<rr::xr::EglContext> egl; // outlives the host: the session ends on a live context
    try {
        const std::filesystem::path discPath = rr::platform::FindDiscImage();
        if (discPath.empty())
            throw std::runtime_error("no disc image in " + rr::platform::DataRoot().string() +
                                     " - copy your own Road Rash: Jailbreak (USA) .bin there as disc.bin "
                                     "(scripts\\install-quest-player.ps1)");
        std::printf("disc: %s\n", discPath.string().c_str());
        // the command line: the disc, then rrgame_args.txt's switches; the VR host's own taken out here
        std::vector<std::string> words = {"rrgame", discPath.string()};
        for (const std::string& w : ReadArgs(rr::platform::DataRoot() / "rrgame_args.txt")) words.push_back(w);
        rrgame::VrHostConfig config;
        std::vector<std::string> kept;
        bool scripted = false;
        for (const std::string& w : words) scripted = scripted || w == "--frames" || w == "--shell-frames";
        // an interactive run starts from the product's defaults (vr_settings.h ProductDefaults), the switches on top,
        // then the file's [vr] (the host)
        if (!scripted) rrgame::VrPrefs() = rrgame::VrSettings::ProductDefaults();
        {
            std::vector<char*> raw;
            for (std::string& w : words) raw.push_back(w.data());
            const int n = static_cast<int>(raw.size());
            for (int i = 0; i < n; ++i) {
                const std::string a = raw[static_cast<size_t>(i)];
                if (i > 0 && a == "--vr") continue; // always VR here
                if (i > 0 && a == "--vr-mock") { config.mock = true; continue; }
                if (i > 0 && a == "--vr-mock-yaw" && i + 1 < n) { config.mockYaw = std::strtof(raw[static_cast<size_t>(++i)], nullptr); continue; }
                if (i > 0 && a == "--vr-mock-pitch" && i + 1 < n) { config.mockPitch = std::strtof(raw[static_cast<size_t>(++i)], nullptr); continue; }
                if (i > 0 && a == "--vr-mock-roll" && i + 1 < n) { config.mockRoll = std::strtof(raw[static_cast<size_t>(++i)], nullptr); continue; }
                if (i > 0 && a == "--vr-mock-pos" && i + 1 < n) {
                    std::sscanf(raw[static_cast<size_t>(++i)], "%f,%f,%f", &config.mockPos[0], &config.mockPos[1], &config.mockPos[2]);
                    continue;
                }
                if (i > 0 && a == "--vr-mock-eye" && i + 1 < n) {
                    std::sscanf(raw[static_cast<size_t>(++i)], "%dx%d", &config.mockEyeWidth, &config.mockEyeHeight);
                    continue;
                }
                if (i > 0 && a == "--vr-menu-shot" && i + 1 < n) { config.menuShotPage = std::atoi(raw[static_cast<size_t>(++i)]); continue; }
                if (i > 0 && rrgame::ApplyVrFlag(n, raw.data(), i, rrgame::VrPrefs())) continue;
                if (a == "--frames" || a == "--shell-frames") scripted = true;
                kept.push_back(a);
            }
        }
        config.scripted = scripted;
        std::string line = "rrgame: command line";
        for (const std::string& k : kept) line += " " + k;
        std::printf("%s%s\n", line.c_str(), config.mock ? " (the VR mock: offscreen eyes, no session)" : "");
        egl = std::make_unique<rr::xr::EglContext>();
        rr::render::LoadGl();
        config.eglDisplay = egl->Display();
        config.eglConfig = egl->Config();
        config.eglContext = egl->Context();
        config.androidVm = app->activity->vm;
        config.androidActivity = app->activity->clazz;
        config.platformPump = [app](int timeoutMs) { return Pump(app, timeoutMs); };
        rrgame::SetHost(rrgame::CreateVrHost(config));
        std::vector<char*> argv;
        for (std::string& k : kept) argv.push_back(k.data());
        const int argc = static_cast<int>(argv.size());
        argv.push_back(nullptr);
        code = rr::shell::GameMain(argc, argv.data(), RaceMainLoop);
        rrgame::SetHost(nullptr); // the session ends (and its swapchains go) while the EGL context still exists
    } catch (const std::exception& e) {
        std::printf("rrgame: FAILED: %s\n", e.what());
        __android_log_print(ANDROID_LOG_ERROR, kTag, "FAILED: %s", e.what());
        rrgame::SetHost(nullptr);
        code = 1;
    }
    egl.reset();
    std::printf("rrgame: exit %d\n", code);
    app->activity->vm->DetachCurrentThread();
    ANativeActivity_finish(app->activity);
    while (Pump(app, 100)) {
    }
}
