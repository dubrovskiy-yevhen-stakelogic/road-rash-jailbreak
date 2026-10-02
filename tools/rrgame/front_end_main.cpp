// rrgame's front end: `rrgame <disc.bin>` with nothing else
// opens the original's shell - boot screens, main menu, modes, career setup, options - run by the
// PORTED shell logic (src\game\shell); a race it starts is rrgame's own race path (`RaceMain`, the
// former `main` of main.cpp) with the --race the handover names, and the race returns here.
//
// Every other command line goes to RaceMain unchanged, so the race switches, the checks and the scripted runs
// work without the front end. Development switches of the front end itself:
//   --shell-frames N          stop after N shell frames (a scripted run: fixed 60 Hz steps)
//   --shell-script "f:keys;..." key presses at shell frames (keys: up down left right x t s c; g<n> screen n,
//                             v<n> career venue n - a saved career's progress, e.g. v5 = Jailbreak)
//   --shell-shot <file.png>   the window's last frame as a PNG
//   --shell-log <file>        one line per shell frame the script or a screen change touches
//   --shell-race "<args>"     more rrgame switches for a scripted race (e.g. "--frames 3000 --autosteer")
//   --shell-card <file>       the memory card image to use instead of saves\rrjb_card.mcr next to rrgame.exe
//   --shell-frame <file.png>  the shell's own last frame at its drawn size (512 x 240, or the HD scale's) as a PNG
//   --hd-media 0|1            HD media (docs\HD-MEDIA.md, hd_media.h) for this run - a scripted run is off otherwise
//   --jailbreak               straight into the Jailbreak mode (career venue 5) through the menus' own path
//
// Portable: the window, the keyboard, the controllers and the
// presentation are the host's (game_host.h). In VR (game_host_vr.h) the shell's picture - menus, films, loading and
// results screens - hangs in front of the player on the theatre quad, at the shell's own 60 Hz.
#include "game/shell/front_end.h"
#include "game/shell/shell_sound.h"
#include "game/audio/mixer.h"
#include "platform/app_paths.h"
#include "platform/png.h"
#include "platform/audio_device.h"
#include "platform/gamepad_state.h"
#include "render/gl_api.h"
#include "render/shaders.h"
#include "render/frame_shot.h"
#include "render/text_overlay.h"
#include "graphics_settings.h"
#include "game_host.h"
#include "game_host_vr.h"
#include "pc_overlay.h"
#include "cheat_menu.h"
#include "handling_settings.h"
#include "hd_media.h"
#include "rrformats/hd_pack.h"
#include "render/render_target.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace rr::render;

namespace {

const char* const kShellVs = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUv;
out vec2 vUv;
void main() { vUv = aUv; gl_Position = vec4(aPos, 0.0, 1.0); }
)";
const char* const kShellFs = R"(#version 330 core
in vec2 vUv;
uniform sampler2D uFrame;
out vec4 oColor;
void main() { oColor = vec4(texture(uFrame, vUv).rgb, 1.0); }
)";

// The shell's 512 x 240 picture, stretched over the window. HD media (hd_media.h): a frame `scale` times larger
// (ShellView::SetScale) is filtered linearly with mipmaps - the original one stays nearest.
struct ShellScreen {
    GLuint program = 0, vao = 0, vbo = 0, texture = 0;
    GLint location = -1;
    int texW = rr::shell::ShellView::kWidth, texH = rr::shell::ShellView::kHeight;
    void Init() {
        texW = rr::shell::ShellView::kWidth;
        texH = rr::shell::ShellView::kHeight;
        program = BuildProgram(kShellVs, kShellFs);
        location = gl.GetUniformLocation(program, "uFrame");
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rr::shell::ShellView::kWidth, rr::shell::ShellView::kHeight, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        const float quad[6][4] = {{-1, 1, 0, 0}, {1, 1, 1, 0}, {-1, -1, 0, 1}, {1, 1, 1, 0}, {1, -1, 1, 1}, {-1, -1, 0, 1}};
        gl.GenVertexArrays(1, &vao);
        gl.BindVertexArray(vao);
        gl.GenBuffers(1, &vbo);
        gl.BindBuffer(GL_ARRAY_BUFFER, vbo);
        gl.BufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
        gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * static_cast<GLsizei>(sizeof(float)), reinterpret_cast<void*>(0));
        gl.EnableVertexAttribArray(0);
        gl.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * static_cast<GLsizei>(sizeof(float)),
                               reinterpret_cast<void*>(sizeof(float) * 2));
        gl.EnableVertexAttribArray(1);
    }
    void Draw(const std::vector<uint8_t>& rgba, int frameW, int frameH, int width, int height) {
        glViewport(0, 0, width, height);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        gl.UseProgram(program);
        gl.ActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        if (frameW != texW || frameH != texH) { // the HD frame's size came or went
            const bool hd = frameW != rr::shell::ShellView::kWidth;
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, frameW, frameH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, hd ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, hd ? GL_LINEAR : GL_NEAREST);
            texW = frameW;
            texH = frameH;
        }
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frameW, frameH, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        if (frameW != rr::shell::ShellView::kWidth) rr::render::GenerateMipmap(GL_TEXTURE_2D);
        gl.Uniform1i(location, 0);
        gl.BindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glEnable(GL_DEPTH_TEST);
    }
};

struct Script {
    std::vector<std::pair<long, std::string>> events;
    void Parse(const std::string& s) {
        for (size_t at = 0; at < s.size();) {
            const size_t semi = s.find(';', at);
            const std::string e = s.substr(at, semi == std::string::npos ? std::string::npos : semi - at);
            const size_t colon = e.find(':');
            if (colon != std::string::npos) events.push_back({std::atol(e.substr(0, colon).c_str()), e.substr(colon + 1)});
            if (semi == std::string::npos) break;
            at = semi + 1;
        }
    }
    bool Has(long frame, const char* key) const {
        for (const auto& e : events)
            if (e.first == frame && ("," + e.second + ",").find(std::string(",") + key + ",") != std::string::npos) return true;
        return false;
    }
};

// The films' sound (movie_player.h): the .WVE's decoded stereo through a mixer and the audio device,
// opened with the first film of an interactive run (a scripted run opens no device) and closed for a race.
struct FilmSound final : rr::platform::AudioSink {
    rr::audio::Mixer mixer{44100, 4};
    std::unique_ptr<rr::platform::AudioDevice> device;
    rr::audio::VoiceId voice = 0;
    void Render(int16_t* out, size_t frames) override { mixer.Mix(out, frames); }
    void Handle(const rr::shell::FrontEnd::MovieAudio& a, bool scripted) {
        if ((a.stop || a.start) && voice != 0) {
            mixer.Stop(voice);
            voice = 0;
        }
        if (!a.start || a.pcm == nullptr || a.pcm->empty() || a.channels <= 0 || scripted) return;
        voice = mixer.Play(rr::audio::VoiceDesc{std::make_shared<rr::audio::MemorySource>(*a.pcm, a.channels, a.rate)});
        if (!device) {
            try {
                device = rr::platform::OpenAudioDevice(*this, mixer.OutputRate());
                device->Start();
            } catch (const std::exception& e) {
                std::fprintf(stderr, "film sound: no audio device (%s)\n", e.what());
            }
        }
    }
};

// A game controller's buttons as the menu's (PlayStation layout: Cross confirm, Triangle back, Square,
// Circle; the d-pad or the left stick moves). Player 1's is the first controller, player 2's the second
// (platform/gamepad_win32.h Poll(1)); a scripted run reads none (frameLimit set), so a pad on the desk
// cannot change a gate.
void AddPad(rr::shell::ShellKeys& k, const rr::platform::GamepadState& gp) {
    if (!gp.connected) return;
    k.up = k.up || gp.up;
    k.down = k.down || gp.down;
    k.left = k.left || gp.left;
    k.right = k.right || gp.right;
    k.cross = k.cross || gp.cross;
    k.triangle = k.triangle || gp.triangle;
    k.square = k.square || gp.square;
    k.circle = k.circle || gp.circle;
}

// Edge-free held state of the keyboard the front end reads (front_end.h ShellKeys).
rr::shell::ShellKeys Keys(const bool* key, const Script& script, long frame) {
    rr::shell::ShellKeys k;
    k.up = key[VK_UP] || script.Has(frame, "up");
    k.down = key[VK_DOWN] || script.Has(frame, "down");
    k.left = key[VK_LEFT] || script.Has(frame, "left");
    k.right = key[VK_RIGHT] || script.Has(frame, "right");
    k.cross = key[VK_RETURN] || key[VK_SPACE] || script.Has(frame, "x");
    k.triangle = key[VK_ESCAPE] || key[VK_BACK] || script.Has(frame, "t");
    k.square = key['O'] || script.Has(frame, "s");
    k.circle = key['H'] || script.Has(frame, "c");
    return k;
}

// Player 2's keys in the menus (pad record 1, which the two-player screens read - screen 31 takes its
// Race and P 2 Bike from pad 1 only): I / K / J / L move, M = Cross, N = Triangle, U = Square, Y = Circle;
// script keys p2up p2down p2left p2right p2x p2t p2s p2c. (In the race player 2 drives with the arrows.)
rr::shell::ShellKeys Keys2(const bool* key, const Script& script, long frame) {
    rr::shell::ShellKeys k;
    k.up = key['I'] || script.Has(frame, "p2up");
    k.down = key['K'] || script.Has(frame, "p2down");
    k.left = key['J'] || script.Has(frame, "p2left");
    k.right = key['L'] || script.Has(frame, "p2right");
    k.cross = key['M'] || script.Has(frame, "p2x");
    k.triangle = key['N'] || script.Has(frame, "p2t");
    k.square = key['U'] || script.Has(frame, "p2s");
    k.circle = key['Y'] || script.Has(frame, "p2c");
    return k;
}

} // namespace

namespace rr::shell {

int GameMain(int argc, char** argv, int (*raceMain)(int, char**)) {
    // The front end only for `rrgame <disc>` plus its own development switches.
    bool frontEnd = argc >= 2 && argv[1][0] != '-';
    long frameLimit = -1;
    std::string shotPath, logPath, raceExtra, cardPath, framePath;
    Script script;
    for (int i = 2; i < argc && frontEnd; ++i) {
        if (std::strcmp(argv[i], "--shell-frames") == 0 && i + 1 < argc) frameLimit = std::atol(argv[++i]);
        else if (std::strcmp(argv[i], "--shell-script") == 0 && i + 1 < argc) script.Parse(argv[++i]);
        else if (std::strcmp(argv[i], "--shell-shot") == 0 && i + 1 < argc) shotPath = argv[++i];
        else if (std::strcmp(argv[i], "--shell-log") == 0 && i + 1 < argc) logPath = argv[++i];
        else if (std::strcmp(argv[i], "--shell-race") == 0 && i + 1 < argc) raceExtra = argv[++i];
        else if (std::strcmp(argv[i], "--shell-card") == 0 && i + 1 < argc) cardPath = argv[++i];
        else if (std::strcmp(argv[i], "--shell-frame") == 0 && i + 1 < argc) framePath = argv[++i];
        else if (std::strcmp(argv[i], "--hd-media") == 0 && i + 1 < argc) // HD media (hd_media.h), as a graphics flag
            rrgame::ApplyGraphicsFlag(argc, argv, i, rrgame::Graphics());
        else if (std::strcmp(argv[i], "--jailbreak") == 0) // the menu path to Jailbreak, pressed for the player:
            // main menu -> Solo -> Jail Break (the gangs film skipped) -> Continue -> the career hub, whose venue
            // is set to 5 as a card whose career reached Jailbreak holds it (session+0x04, rules.md 2.3) -> Race
            script.Parse("2:g4;6:x;14:x;20:x;30:x;37:v5;38:down;40:up;44:x");
        else frontEnd = false;
    }
    if (!frontEnd) return raceMain(argc, argv);

    try {
        DiscImage disc(argv[1]);
        const bool scripted = frameLimit >= 0;
        // The seed stands in for the root counter FrontendInit reads (shell_arena.h); a scripted run is fixed.
        const uint32_t seed = scripted ? 0u : static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                                         std::chrono::steady_clock::now().time_since_epoch())
                                                                         .count());
        FrontEnd fe(disc, seed);
        {
            // The career card lives next to rrgame.exe (shell_card.h): rrjb_card.mcr.
            const std::string dir = rr::platform::ExecutableDir().string();
            // Saves and settings live in saves\ next to rrgame.exe (the installed layout, scripts\install.ps1) - on
            // the Quest in the app's internal files/saves (platform/app_paths.h SavesDir, docs\QUEST.md).
            // A card from before that (rrjb_card.mcr beside the exe) is copied there once and left in place.
            if (cardPath.empty()) {
                std::error_code ec;
                const std::filesystem::path saves = rr::platform::SavesDir();
                std::filesystem::create_directories(saves, ec);
                const std::filesystem::path card = saves / "rrjb_card.mcr", old = std::filesystem::path(dir) / "rrjb_card.mcr";
                if (!std::filesystem::exists(card, ec) && std::filesystem::is_regular_file(old, ec) &&
                    std::filesystem::copy_file(old, card, ec))
                    std::printf("card: %s copied to %s (the old file is kept)\n", old.string().c_str(), card.string().c_str());
                cardPath = card.string();
            }
            fe.SetCardPath(cardPath);
        }
        // the cheat menu (cheat_menu.h): its settings (a scripted front end: all off), the disc's weapon
        // list, and the disc and card the career cheats use
        rrgame::StartCheats(scripted);
        rrgame::StartHandlingFrontEnd(scripted); // the menus over the front end show the saved handling
        if (!scripted) std::printf("%s", rrgame::LoadCheatWeapons(disc).c_str());
        else rrgame::LoadCheatWeapons(disc);
        rrgame::SetCheatDisc(&disc);
        rrgame::SetCheatCardPath(cardPath);
        std::printf("rrgame: the front end (the ported shell of RASHCDF)\n%s", fe.ArenaReport().c_str());
        std::printf("keys: arrows move / change, Enter = Cross (confirm), Esc or Backspace = Triangle (back),\n"
                    "      O = Square (options), H = Circle (help); closing the window quits\n");
        rrgame::GameHost& host = rrgame::Host();
        host.OpenWindow(1280, 720);
        const bool* keyboard = host.Keys();
        rrgame::VrHost* const vr = host.Vr(); // the theatre quad (game_host_vr.h); null on the desktop
        ShellScreen screen;
        // The PC settings (graphics_settings.h) and the F10 / Select + Start overlay (pc_overlay.h) over the menus: the
        // menus hold while it is open. A scripted front end reads no settings file and opens no overlay.
        rrgame::GraphicsSettings& gfx = rrgame::StartGraphics(scripted);
        rrgame::OpenHdPack(disc); // HD media (hd_media.h): the pack of this disc, when one is installed
        rr::render::TextOverlay overlayText;
        rrgame::PcOverlay overlay;
        overlay.SetContext(false);
        rrgame::FrameProfiler menuProfiler;
        bool overlaySwallow = false, appliedVsync = false, appliedFull = false, firstApply = true;
        const auto applyWindow = [&]() {
            if (scripted) return;
            if (firstApply || gfx.vsync != appliedVsync) host.SetVsync(gfx.vsync);
            if (firstApply || gfx.fullscreen != appliedFull) host.SetFullscreen(gfx.fullscreen);
            appliedVsync = gfx.vsync;
            appliedFull = gfx.fullscreen;
            firstApply = false;
        };
        applyWindow();
        const auto drawOverlay = [&](int width, int height) { // the overlay and the profiler at the window's size
            if (scripted || (!overlay.Open() && !gfx.profiler)) return;
            if (!overlayText.Ready()) overlayText.Init();
            rrgame::ProfileNumbers numbers;
            numbers.frames = &menuProfiler;
            numbers.renderW = width;
            numbers.renderH = height;
            overlay.Draw(overlayText, width, height, numbers);
            glViewport(0, 0, width, height);
        };
        screen.Init();
        std::string log;
        std::vector<uint8_t> rgba, lastShot;
        int frameW = rr::shell::ShellView::kWidth, frameH = rr::shell::ShellView::kHeight; // what fe.Draw drew (HD media)
        int lastW = 0, lastH = 0, lastScreen = -1;
        long frame = 0;
        auto next = std::chrono::steady_clock::now();
        host.SetEscapeQuits(false);
        FilmSound filmSound;
        ShellSound shellSound(disc); // the UI clicks and the menu music (shell_sound.h)
        const std::unique_ptr<rrgame::PadDevice> menuPad1 = host.OpenPad(), menuPad2 = host.OpenPad(); // AddPad
        if (!shellSound.Error().empty()) std::fprintf(stderr, "shell sound: %s\n", shellSound.Error().c_str());
        // VR: the shell keeps its own 60 Hz on the steady clock while the compositor asks for frames at the display's
        // rate - between the shell's frames the theatre shows its last picture again (game_host_vr.h IdleFrame)
        auto shellClock = std::chrono::steady_clock::now();
        long shellFields = 0;
        const auto drawShell = [&](int w, int h) {
            if (!rgba.empty()) screen.Draw(rgba, frameW, frameH, w, h);
            else {
                glViewport(0, 0, w, h);
                glClearColor(0, 0, 0, 1);
                glClear(GL_COLOR_BUFFER_BIT);
            }
        };
        while (host.Pump()) {
            if (vr != nullptr) {
                if (vr->MenuHolds()) { // the VR settings menu over the menus: the shell holds
                    vr->TheatreFrame(drawShell);
                    shellClock = std::chrono::steady_clock::now();
                    shellFields = 0;
                    continue;
                }
                if (!scripted) {
                    const long due = static_cast<long>(
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - shellClock).count() * 60.0);
                    if (due - shellFields > 3) shellFields = due - 1; // a stall (a load): no catch-up burst
                    if (shellFields >= due) {
                        vr->IdleFrame();
                        continue;
                    }
                    ++shellFields;
                }
            }
            ++frame;
            ShellKeys keys = Keys(keyboard, script, frame);
            rr::platform::GamepadState menu1, menu2; // an XInput pad can vibrate (front_end.h)
            if (!scripted) {
                menu1 = menuPad1->Poll(0);
                AddPad(keys, menu1); // the first controller (AddPad)
            }
            if (!scripted && vr == nullptr) { // the settings overlay: while it is open, and until the keys that closed it are let go
                const bool open = overlay.Update(keyboard, menu1);
                if (open) {
                    overlaySwallow = true;
                } else if (overlaySwallow) {
                    using rr::platform::PadButton;
                    overlaySwallow = keyboard[VK_ESCAPE] || keyboard[VK_RETURN] || keyboard[VK_SPACE] ||
                                     keyboard[VK_BACK] || keyboard[VK_F10] || menu1.raw.Down(PadButton::Start) ||
                                     menu1.raw.Down(PadButton::Select) || menu1.raw.Down(PadButton::Triangle) ||
                                     menu1.raw.Down(PadButton::Cross);
                }
                if (overlay.TakeChanged()) applyWindow();
                if (overlaySwallow) {
                    int width = 0, height = 0;
                    host.FrameSize(width, height);
                    if (!rgba.empty()) screen.Draw(rgba, frameW, frameH, width, height);
                    drawOverlay(width, height);
                    host.Present();
                    menuProfiler.Record(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
                    next += std::chrono::microseconds(16667);
                    std::this_thread::sleep_until(next);
                    continue;
                }
            }
            for (const auto& e : script.events) // development: "g<n>" = GotoScreen(n), the boot's own entry
                if (e.first == frame && e.second.size() > 1 && e.second[0] == 'g') GotoScreen(fe.Ram(), std::atoi(e.second.c_str() + 1));
                else if (e.first == frame && e.second.size() > 1 && e.second[0] == 'v') // development: "v<n>" = career venue session+0x04 (a card's saved progress)
                    fe.Ram().W8(0x800D80DCu, static_cast<uint8_t>(std::atoi(e.second.c_str() + 1)));
            ShellKeys keys2 = Keys2(keyboard, script, frame); // player 2's pad
            if (!scripted) {
                menu2 = menuPad2->Poll(1);
                AddPad(keys2, menu2); // the second controller
            }
            const char* hidden = std::getenv("RRJB_WINDOW"); // a hidden (scripted) run drives no motor
            const bool quiet = hidden != nullptr && std::strcmp(hidden, "hidden") == 0;
            fe.SetVibrationPads(!quiet && menu1.connected && menu1.hasMotors, // XInput or a DualSense
                                !quiet && menu2.connected && menu2.hasMotors);
            // HD media (hd_media.h): the switch as the settings hold it now - the VR setting in the headset, the desktop's
            // F10 one otherwise (a scripted run: its --hd-media only) - and the shell's draw scale with it, before the
            // frame (a film the frame opens takes the scale it opens with).
            rrgame::ApplyHdSwitch(vr != nullptr && !scripted ? vr->Settings().hdMedia : gfx.hdMedia);
            fe.SetHdScale(rrgame::HdShellScale());
            fe.Frame(keys, &keys2);
            const int cur = fe.CurrentScreen();
            const bool touched = cur != lastScreen || keys.up || keys.down || keys.left || keys.right || keys.cross ||
                                 keys.triangle || keys.square || keys.circle || keys2.up || keys2.down || keys2.left ||
                                 keys2.right || keys2.cross || keys2.triangle || keys2.square || keys2.circle;
            if (touched || !fe.Callees().unported.empty()) {
                char head[32];
                std::snprintf(head, sizeof(head), "f%-6ld ", frame);
                log += head + fe.Status() + "\n";
            }
            lastScreen = cur;
            if (const std::string note = fe.TakeCardNote(); !note.empty()) {
                std::printf("%s\n", note.c_str());
                log += note + "\n";
            }
            filmSound.Handle(fe.TakeMovieAudio(), scripted); // the films' sound (movie_player.h)
            // The clicks and the music this frame asked for, through the films' mixer (shell_sound.h).
            shellSound.Frame(fe.Ram(), fe.Callees().uiSounds, fe.Callees().musicRequest, fe.Callees().musicStop,
                             filmSound.mixer);
            if (!scripted && !filmSound.device && shellSound.Ready()) {
                try {
                    filmSound.device = rr::platform::OpenAudioDevice(filmSound, filmSound.mixer.OutputRate());
                    filmSound.device->Start();
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "shell sound: no audio device (%s)\n", e.what());
                    shellSound.Stop(filmSound.mixer);
                }
            }
            if (const std::string note = fe.TakeMovieNote(); !note.empty()) {
                std::printf("%s\n", note.c_str());
                log += note + "\n";
            }
            fe.Draw(rgba);
            frameW = fe.FrameWidth();
            frameH = fe.FrameHeight();
            if (rgba.size() != static_cast<size_t>(frameW) * static_cast<size_t>(frameH) * 4u) { // a film or results frame drawn at another scale
                frameW = rr::shell::ShellView::kWidth;
                frameH = static_cast<int>(rgba.size() / 4u / static_cast<size_t>(frameW));
            }
            const bool last = scripted && frame >= frameLimit;
            if (vr != nullptr) { // the shell's picture on the theatre quad (the shot: the quad as the headset gets it)
                if (last && !shotPath.empty()) vr->RequestShot(shotPath);
                vr->TheatreFrame(drawShell);
            } else {
                int width = 0, height = 0;
                host.FrameSize(width, height);
                screen.Draw(rgba, frameW, frameH, width, height);
                drawOverlay(width, height); // the profiler (pc_overlay.h)
                if (!scripted)
                    menuProfiler.Record(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
                if (last && !shotPath.empty()) {
                    glFinish();
                    lastShot = ReadFrame(width, height);
                    lastW = width;
                    lastH = height;
                }
                host.Present();
            }
            if (last) break;

            if (fe.RaceRequested()) {
                shellSound.Stop(filmSound.mixer);
                filmSound.device.reset(); // the race opens its own device
                // THE HANDOVER: game_state+0x00 = 3 (StartRaceInput 0x8006C354); the ported commit
                // 0x8007F37C fills game_state; the race runs on what it wrote.
                Handover& h = PendingHandover();
                fe.BeginRace(h);
                char line[256];
                std::snprintf(line, sizeof(line),
                              "f%-6ld HANDOVER: race set %d id %d, race type 0x%02X, bank %u, player bike %s\n", frame,
                              h.set, h.raceId, h.gameState[4], h.gameState[0x3C], h.playerBikePh.c_str());
                std::printf("%s", line);
                log += line;
                std::string set = std::to_string(h.set), id = std::to_string(h.raceId);
                std::vector<char*> args = {argv[0], argv[1], const_cast<char*>("--race"), set.data(), id.data()};
                // A scripted front end runs a scripted race: 300 frames, then back.
                std::string raceFrames = "300", raceLog = logPath.empty() ? std::string() : logPath + ".race.txt";
                if (scripted) {
                    args.push_back(const_cast<char*>("--frames"));
                    args.push_back(raceFrames.data());
                    if (!raceLog.empty()) {
                        args.push_back(const_cast<char*>("--log"));
                        args.push_back(raceLog.data());
                    }
                }
                std::vector<std::string> extra; // --shell-race: split on spaces, after the defaults (a later switch wins)
                for (size_t at = 0; scripted && at < raceExtra.size();) {
                    const size_t sp = raceExtra.find(' ', at);
                    const std::string w = raceExtra.substr(at, sp == std::string::npos ? std::string::npos : sp - at);
                    if (!w.empty()) extra.push_back(w);
                    if (sp == std::string::npos) break;
                    at = sp + 1;
                }
                for (std::string& w : extra) args.push_back(w.data());
                host.SetEscapeQuits(false);
                const int rc = raceMain(static_cast<int>(args.size()), args.data());
                const bool closed = host.Closed();
                std::snprintf(line, sizeof(line), "        back from the race (exit %d): game_state+0x00 %u, the player's "
                              "result %u, stamp %u ticks%s\n", rc, h.gameStateByte, h.resultCode, h.resultTicks,
                              closed ? ", the window was closed" : "");
                std::printf("%s", line);
                log += line;
                fe.EndRace(h);
                h.active = false;
                if (closed) break;
                screen.Init(); // the race left its own GL state bound
                firstApply = true; // the race may have changed the settings (graphics_settings.h)
                applyWindow();
                next = std::chrono::steady_clock::now();
                shellClock = next;
                shellFields = 0;
            }
            if (!scripted && vr == nullptr) { // the shell's pace: one frame per 60 Hz vertical blank
                next += std::chrono::microseconds(16667);
                std::this_thread::sleep_until(next);
            }
        }
        if (!lastShot.empty()) SaveShot(shotPath, lastW, lastH, lastShot);
        if (!framePath.empty() && !rgba.empty()) { // --shell-frame: the frame as the shell drew it (HD media: its full size)
            rr::WritePng(framePath, frameW, frameH, rgba);
            std::printf("shell frame: %dx%d -> %s\n", frameW, frameH, framePath.c_str());
        }
        for (const std::string& m : fe.View().Missing()) log += "view: " + m + "\n";
        if (rr::hd::LoadedPack()) { // HD media: what the shell drew from the pack, and what the pack refused
            std::printf("%s\n", fe.View().HdReport().c_str());
            log += fe.View().HdReport() + "\n";
            for (const std::string& n : rr::hd::LoadedPack()->TakeNotes()) {
                std::printf("hd: %s\n", n.c_str());
                log += "hd: " + n + "\n";
            }
        }
        { // the panel films and logos (shell_panel.h; RRJB_MENU_FILMS=off is the control)
            char line[512];
            std::snprintf(line, sizeof(line), "menu panels: %llu film-widget calls, %llu logo calls (PORTED 0x8006E4D8 / "
                          "0x8006E8FC); %s\n", static_cast<unsigned long long>(fe.Callees().filmGates),
                          static_cast<unsigned long long>(fe.Callees().logoCalls), fe.Films().Report().c_str());
            std::printf("%s", line);
            log += line;
        }
        if (!logPath.empty())
            if (FILE* out = std::fopen(logPath.c_str(), "wb")) {
                std::fwrite(log.data(), 1, log.size(), out);
                std::fclose(out);
                std::printf("front-end log: %s\n", logPath.c_str());
            }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}

} // namespace rr::shell
