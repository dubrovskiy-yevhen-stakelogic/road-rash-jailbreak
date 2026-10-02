// rrgame's entry on Windows: the disc lookup, the scripted-run policy and the host (game_host.h).
//
//   rrgame [<disc>] [...]              the desktop game (the Win32 window)
//   rrgame [<disc>] --vr [...]         the whole game in a PCVR headset through OpenXR (game_host_vr.h); the runtime:
//                                      RRJB_XR_RUNTIME=meta | steamvr | vdxr (this process only; the PLAY-PCVR-*.bat
//                                      launchers set it) or the system's active one
//   rrgame [<disc>] --vr-mock [...]    the same VR frames into offscreen eyes with a synthetic head, no headset: the
//                                      desktop verification of the VR path (--shot writes both eyes side by side and
//                                      the theatre quad below). --vr-mock-yaw D / --vr-mock-pitch D / --vr-mock-roll D
//                                      turn the head, --vr-mock-pos X,Y,Z moves it (metres, OpenXR axes: x right,
//                                      y up, z back), --vr-mock-eye WxH sizes the eyes, --vr-menu-shot P draws the VR
//                                      menu open on page P
//   VR settings flags (vr_settings.h ApplyVrFlag): --vr-eye-scale, --vr-refresh, --vr-msaa, --vr-horizon-lock,
//   --vr-view head|chase, --vr-comfort, --vr-seat, --vr-hud, --vr-foveation, --vr-draw-distance, --vr-max-detail,
//   --vr-multiview (single-pass stereo 0|1), --vr-textures-smooth 0|1.
#include "game/shell/front_end.h"
#include "game_host.h"
#include "game_host_vr.h"
#include "render/window_win32.h"
#include "rrvfs/disc_identity.h"
#include "settings_check.h"
#include "vr_settings.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

int RaceMainLoop(int argc, char** argv); // main.cpp

int main(int argc, char** argv) {
    rr::render::HideIfScriptedRun(argc, argv); // a script never gets a window, focus or sound
    if (argc >= 3 && std::strcmp(argv[1], "--settings-roundtrip-check") == 0) return rrgame::SettingsRoundTripCheck(argv[2], argc >= 4 && std::strcmp(argv[3], "start") == 0);
    if (argc >= 3 && std::strcmp(argv[1], "--vr-settings-check") == 0) {
        // a settings file's [vr] section read as the headset reads it (LoadVrSettings, with its one-time
        // migrations), and the section the next save would write - nothing is written
        rrgame::VrSettings s;
        if (!rrgame::LoadVrSettings(argv[2], s)) {
            std::fprintf(stderr, "rrgame: --vr-settings-check: cannot read %s\n", argv[2]);
            return 1;
        }
        std::printf("vr settings check: %s - combat %s (the buttons %s)\nthe next save writes:\n%s", argv[2],
                    rrgame::MeleeModeName(s.melee.mode), s.melee.ButtonsAttack() ? "attack" : "do not attack",
                    s.melee.Serialize().c_str());
        return 0;
    }
    // An interactive VR run starts from the product's defaults (vr_settings.h ProductDefaults), the command line's
    // --vr-* on top, then the file's [vr] (the host); a scripted one (hidden) from the member initialisers.
    {
        const char* w = std::getenv("RRJB_WINDOW");
        const bool scripted = w != nullptr && std::strcmp(w, "hidden") == 0;
        bool vrRun = false;
        for (int i = 1; i < argc; ++i) vrRun = vrRun || std::strcmp(argv[i], "--vr") == 0 || std::strcmp(argv[i], "--vr-mock") == 0;
        if (vrRun && !scripted) rrgame::VrPrefs() = rrgame::VrSettings::ProductDefaults();
    }
    // The VR switches are the host's, not the game's: taken out of the command line before anything else reads it.
    bool vr = false, mock = false;
    rrgame::VrHostConfig vrConfig;
    std::vector<char*> kept;
    try {
        for (int i = 0; i < argc; ++i) {
            if (i > 0 && std::strcmp(argv[i], "--vr") == 0) { vr = true; continue; }
            if (i > 0 && std::strcmp(argv[i], "--vr-mock") == 0) { vr = mock = true; continue; }
            if (i > 0 && std::strcmp(argv[i], "--vr-mock-yaw") == 0 && i + 1 < argc) { vrConfig.mockYaw = std::strtof(argv[++i], nullptr); continue; }
            if (i > 0 && std::strcmp(argv[i], "--vr-mock-pitch") == 0 && i + 1 < argc) { vrConfig.mockPitch = std::strtof(argv[++i], nullptr); continue; }
            if (i > 0 && std::strcmp(argv[i], "--vr-mock-roll") == 0 && i + 1 < argc) { vrConfig.mockRoll = std::strtof(argv[++i], nullptr); continue; }
            if (i > 0 && std::strcmp(argv[i], "--vr-mock-pos") == 0 && i + 1 < argc) { // the head moved: X,Y,Z metres
                if (std::sscanf(argv[++i], "%f,%f,%f", &vrConfig.mockPos[0], &vrConfig.mockPos[1], &vrConfig.mockPos[2]) != 3)
                    throw std::runtime_error("--vr-mock-pos: X,Y,Z");
                continue;
            }
            if (i > 0 && std::strcmp(argv[i], "--vr-mock-eye") == 0 && i + 1 < argc) {
                if (std::sscanf(argv[++i], "%dx%d", &vrConfig.mockEyeWidth, &vrConfig.mockEyeHeight) != 2)
                    throw std::runtime_error("--vr-mock-eye: WxH");
                continue;
            }
            if (i > 0 && std::strcmp(argv[i], "--vr-menu-shot") == 0 && i + 1 < argc) { vrConfig.menuShotPage = std::atoi(argv[++i]); continue; }
            if (i > 0 && rrgame::ApplyVrFlag(argc, argv, i, rrgame::VrPrefs())) continue;
            kept.push_back(argv[i]);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "rrgame: %s\n", e.what());
        return 2;
    }
    argc = static_cast<int>(kept.size());
    argv = kept.data();
    // The disc: the command line's first argument when it names one (a .cue is followed to its .bin),
    // else the installed one - disc.txt, runtime\disc\*.bin, disc\*.bin next to rrgame.exe
    // (rrvfs/disc_identity.h). The found path goes in as argv[1], so everything below is unchanged.
    std::vector<char*> args(argv, argv + argc);
    std::string disc, source = "command line";
    try {
        if (argc >= 2 && argv[1][0] != '-') {
            disc = rr::ResolveImagePath(argv[1]);
            args[1] = disc.data();
        } else {
            char exe[MAX_PATH] = {};
            GetModuleFileNameA(nullptr, exe, MAX_PATH);
            disc = rr::FindInstalledDisc(std::filesystem::path(exe).parent_path().string(), source);
            if (disc.empty()) std::fprintf(stderr, "rrgame: no disc image given and none installed (searched %s)\n", source.c_str());
            else args.insert(args.begin() + 1, disc.data());
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "rrgame: %s\n", e.what());
        return 1;
    }
    if (!disc.empty() && source != "command line") std::printf("disc: %s (from %s)\n", disc.c_str(), source.c_str());
    const int count = static_cast<int>(args.size());
    args.push_back(nullptr); // argv[argc] == nullptr, as the C runtime gives it
    if (vr) {
        // A scripted VR run (a frame count, a shot: HideIfScriptedRun hid it) reads no settings file.
        const char* w = std::getenv("RRJB_WINDOW");
        vrConfig.scripted = w != nullptr && std::strcmp(w, "hidden") == 0;
        vrConfig.mock = mock;
        // the edge rule's geometry stage has no console grid to decide in an eye image (edge_rule.h): GL's own coverage
        if (std::getenv("RRJB_EDGE") == nullptr) _putenv_s("RRJB_EDGE", "gl");
        std::printf("rrgame: %s\n", mock ? "the desktop VR mock (--vr-mock): the VR frames without a headset"
                                         : "VR (--vr): OpenXR on this PC's GL context");
        rrgame::SetHost(rrgame::CreateVrHost(vrConfig));
    } else {
        rrgame::SetHost(rrgame::CreateDesktopHost());
    }
    return rr::shell::GameMain(count, args.data(), RaceMainLoop);
}
