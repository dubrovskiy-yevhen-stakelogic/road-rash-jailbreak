// rrvrtest (Windows): the PCVR proof of the OpenXR layer.
//
//   rrvrtest <disc.bin> [--race SET ID] [--distance D] [--frames N] [--scale X] [--refresh HZ] [--shot DIR]
//                       [--ps1] [--no-quad] [--mock] [--dump-es-shaders DIR]
//
// Opens the player's disc image, loads the world of the race (default 1 20) with the shared renderer, creates an OpenXR
// session on the GL 3.3 context of a (hidden when scripted) window and draws the road in stereo with head tracking
// plus the theatre quad. RRJB_XR_RUNTIME=meta | vdxr | steamvr picks a runtime for this process only (the system's
// active runtime is never changed).
//   --mock                 no OpenXR: both eyes side by side in the window from a fixed synthetic head (a desktop
//                          check of the eye path - anchor, asymmetric projection, the scene per eye); with --shot
//                          the window is written to DIR/mock_eyes.png.
//   --dump-es-shaders DIR  writes every shared shader (render/shaders.h) as CompileShader passes it to an OpenGL ES
//                          driver (EsShaderSource), with and without the two optional extensions, for glslangValidator.
#include "platform/xr/xr_session_gl.h"
#include "render/edge_rule.h"
#include "render/gl_api.h"
#include "render/shaders.h"
#include "render/window_win32.h"
#include "rrvfs/disc_image.h"
#include "vr_loop.h"
#include "vr_scene.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace {

int DumpEsShaders(const std::string& dir) {
    using namespace rr::render;
    struct Src {
        const char* name;
        GLenum type;
        const char* source;
    };
    const Src sources[] = {
        {"main.vert", GL_VERTEX_SHADER, kVertexShader},       {"main.frag", GL_FRAGMENT_SHADER, kFragmentShader},
        {"sky.vert", GL_VERTEX_SHADER, kSkyVertexShader},     {"sky.frag", GL_FRAGMENT_SHADER, kSkyFragmentShader},
        {"grad.vert", GL_VERTEX_SHADER, kGradVertexShader},   {"grad.frag", GL_FRAGMENT_SHADER, kGradFragmentShader},
        {"cloud.vert", GL_VERTEX_SHADER, kCloudVertexShader}, {"cloud.frag", GL_FRAGMENT_SHADER, kCloudFragmentShader},
        {"shadow.vert", GL_VERTEX_SHADER, kShadowVertexShader}, {"shadow.frag", GL_FRAGMENT_SHADER, kShadowFragmentShader},
        {"post.vert", GL_VERTEX_SHADER, kPostVertexShader},   {"post.frag", GL_FRAGMENT_SHADER, kPostFragmentShader},
        {"hud.vert", GL_VERTEX_SHADER, kHudVertexShader},     {"hud.frag", GL_FRAGMENT_SHADER, kHudFragmentShader},
    };
    std::filesystem::create_directories(dir);
    int written = 0;
    for (int variant = 0; variant < 2; ++variant) {
        GlCaps caps;
        caps.es = true;
        caps.clipDistance = caps.noPerspective = variant == 0;
        const char* tag = variant == 0 ? "ext" : "noext";
        for (const Src& s : sources) {
            const std::string path = dir + "/" + tag + "_" + s.name;
            std::ofstream out(path, std::ios::binary);
            out << EsShaderSource(s.type, s.source, caps);
            ++written;
        }
    }
    std::printf("rrvrtest: %d ES shader sources written to %s\n", written, dir.c_str());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    rr::render::HideIfScriptedRun(argc, argv);
    std::string discPath, shotDir, esDir;
    int set = 1, race = 20;
    float distance = 20.0f, scale = 1.0f, refresh = 0.0f;
    long long frames = -1;
    bool mock = false, ps1 = false, quad = true;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error(a + " needs a value");
            return argv[++i];
        };
        try {
            if (a == "--race") {
                set = std::stoi(next());
                race = std::stoi(next());
            } else if (a == "--distance") distance = std::stof(next());
            else if (a == "--frames") frames = std::stoll(next());
            else if (a == "--scale") scale = std::stof(next());
            else if (a == "--refresh") refresh = std::stof(next());
            else if (a == "--shot") shotDir = next();
            else if (a == "--dump-es-shaders") esDir = next();
            else if (a == "--mock") mock = true;
            else if (a == "--ps1") ps1 = true;
            else if (a == "--no-quad") quad = false;
            else if (!a.empty() && a[0] != '-' && discPath.empty()) discPath = a;
            else throw std::runtime_error("unknown option " + a);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "rrvrtest: %s\n", e.what());
            return 2;
        }
    }
    if (!esDir.empty()) return DumpEsShaders(esDir);
    if (discPath.empty()) {
        std::fprintf(stderr, "usage: rrvrtest <disc.bin> [--race SET ID] [--distance D] [--frames N] [--scale X] "
                             "[--refresh HZ] [--shot DIR] [--ps1] [--no-quad] [--mock] [--dump-es-shaders DIR]\n");
        return 2;
    }
    // The edge rule's geometry stage decides only where one window pixel is one console pixel (edge_rule.h): never
    // in an eye image, so VR builds the programs without it.
    if (!std::getenv("RRJB_EDGE")) _putenv_s("RRJB_EDGE", "gl");
    try {
        rr::render::Window window = rr::render::CreateGlWindow(1280, 720, "rrvrtest", "rrvrtest", nullptr);
        rr::DiscImage disc(discPath);
        rrvr::ProofScene scene(disc, set, race, distance);
        if (mock) {
            // the offscreen mock pair the Quest build draws too (vr_loop.h MockPair), then the stereo timing at the
            // Quest 3's recommended eye size
            rrvr::LoopOptions loop;
            loop.ps1Look = ps1;
            if (!shotDir.empty()) {
                std::filesystem::create_directories(shotDir);
                if (!rrvr::MockPair(scene, 1280, 720, shotDir + "/mock_eyes.png", loop)) throw std::runtime_error("no offscreen target");
            }
            rrvr::MockTiming(scene, 1680, 1760, frames > 0 ? static_cast<int>(frames) : 30, loop);
            (void)window;
            return 0;
        }
        rr::xr::SessionOptions options;
        options.appName = "rrvrtest";
        options.binding.hdc = window.dc;
        options.binding.hglrc = window.rc;
        options.eyeScale = scale;
        options.refreshHz = refresh;
        options.quadYawOffset = 0.6f; // the theatre panel 34 degrees to the left of the road ahead
        rr::xr::GlSession session(options);
        rrvr::LoopOptions loop;
        loop.frames = frames;
        loop.ps1Look = ps1;
        loop.quad = quad;
        loop.shotDir = shotDir;
        if (!shotDir.empty()) std::filesystem::create_directories(shotDir);
        loop.keepGoing = [] { return rr::render::PumpMessages(); };
        return rrvr::RunLoop(scene, session, loop);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "rrvrtest: %s\n", e.what());
        std::printf("rrvrtest: FAILED: %s\n", e.what());
        return 1;
    }
}
