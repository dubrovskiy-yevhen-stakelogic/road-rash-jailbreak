#pragma once
// The PC settings overlay: F10, or Select + Start on a gamepad (Create +
// Options on a DualSense), opens it over the race or the menus; the race does not advance while it is open.
// Up / Down (arrows, d-pad) choose a row, Left / Right change its value, Enter / Cross opens or confirms, Esc /
// Triangle goes back and closes. Every change applies at once and is saved to rrgame_settings.ini (graphics) or
// controls.ini (the input bindings, platform/input_bindings.h).
#include "frame_profiler.h"
#include "graphics_settings.h"
#include "platform/gamepad_state.h"
#include "render/text_overlay.h"

#include <string>

namespace rrgame {

// What the profiler shows (filled by the caller each frame).
struct ProfileNumbers {
    const FrameProfiler* frames = nullptr;
    double cpuMs = -1.0;      // the frame's CPU time (simulation + building the draws), ms
    double renderCpuMs = -1.0; // of which the drawing of the views
    double gpuMs = -1.0;      // the GPU's time for the frame (timer query), ms; < 0 unavailable
    unsigned long long drawCalls = 0, triangles = 0;
    int renderW = 0, renderH = 0, samples = 1;
    // the scene's own counts (race_scene.h RaceScene::FrameStats)
    size_t cellsDrawn = 0, cellsCulled = 0, propsDrawn = 0, propsCulled = 0, objectsDrawn = 0, objectsCulled = 0;
    bool race = false;
};

class PcOverlay {
public:
    // Feeds one frame of input: the window's held keys and the first gamepad. Opens / closes on F10 or Select +
    // Start. Returns true while the overlay is open (the caller then holds the game).
    bool Update(const bool* keys, const rr::platform::GamepadState& pad);
    bool Open() const { return open_; }
    // True once after a setting changed (the caller applies the settings).
    bool TakeChanged() {
        const bool c = changed_;
        changed_ = false;
        return c;
    }
    // Draws the overlay (when open) and the profiler (when on) over the bound window framebuffer of w x h.
    void Draw(rr::render::TextOverlay& text, int w, int h, const ProfileNumbers& numbers);
    // `inRace`: the pages that only matter in a race are marked so in the menus.
    void SetContext(bool inRace) { inRace_ = inRace; }
    // DEVELOPMENT (RRJB_OVERLAY_SHOT=<page>): the overlay drawn open on page 0..5 without taking input, so a scripted
    // run's shot shows it.
    void ShowPage(int page) {
        open_ = true;
        page_ = page < 0 || page > 5 ? 0 : page; // 5: the Cheats page (cheat_menu.h)
        selected_ = 0;
    }

private:
    void Activate(int direction); // Enter (0) / Left (-1) / Right (+1) on the selected row
    void Back();
    void Save();
    int page_ = 0, selected_ = 0, scroll_ = 0;
    bool open_ = false, changed_ = false, inRace_ = true;
    bool keyWas_[256] = {};
    bool padWas_[8] = {};
    bool comboWas_ = false;
    std::string status_;
    int capturing_ = -1; // the Controls page: the action being rebound (a gamepad or a keyboard capture)
    bool captureGamepad_ = true;
    int repeatDir_ = 0;
    double repeatAt_ = 0;
};

// The profiler's panel alone (top-left), for a caller that draws no overlay.
void DrawProfiler(rr::render::TextOverlay& text, int w, int h, const ProfileNumbers& numbers);

} // namespace rrgame
