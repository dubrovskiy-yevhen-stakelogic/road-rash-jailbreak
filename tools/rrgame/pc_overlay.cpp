// The PC settings overlay (pc_overlay.h). The pattern of the PC SETTINGS overlay of the gt2-play project
// (MIT): pages of rows, Left / Right change a value, every change saved at once.
#include "rrformats/hd_pack.h"
#include "pc_overlay.h"

#include "cheat_menu.h" // the Cheats page
#include "handling_settings.h" // the Handling rows
#include "platform/input_bindings.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace rrgame {

namespace {

enum Page { kMain = 0, kResolution, kImage, kDetail, kControls, kCheats, kPages }; // kCheats: cheat_menu.h

// The render resolution choices: percentages of the window, then the fixed heights (encoded as -height).
const int kResolutions[] = {50, 60, 70, 80, 90, 100, 110, 125, 150, 175, 200, -720, -1080, -1440, -2160};
const int kFpsCaps[] = {0, 30, 60, 90, 120, 144, 165, 240};
const int kMsaa[] = {1, 2, 4, 8};

template <size_t N>
int Cycle(int value, const int (&values)[N], int direction) {
    const auto it = std::find(std::begin(values), std::end(values), value);
    const int index = it == std::end(values) ? 0 : static_cast<int>(it - std::begin(values));
    return values[(index + direction + static_cast<int>(N)) % static_cast<int>(N)];
}

std::string OnOff(bool b) { return b ? "On" : "Off"; }

struct Row {
    std::string label, value;
    bool submenu = false;
};

double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

// ---------------------------------------------------------------- the rows of each page
static std::vector<Row> RowsOf(int page, bool inRace, int capturing) {
    const GraphicsSettings& s = Graphics();
    std::vector<Row> r;
    switch (page) {
    case kMain:
        r.push_back({"Resolution and display", "", true});
        r.push_back({"Image quality", "", true});
        r.push_back({"Detail and draw distance", "", true});
        r.push_back({"Controls", "", true});
        r.push_back({"Cheats", rr::game::Cheats().AnyOn() ? "ON" : "", true});
        r.push_back({"Profiler (FPS, frame time)", OnOff(s.profiler)});
        r.push_back({"Preset: modern", s == GraphicsSettings::Modern() ? "(active)" : ""});
        r.push_back({"Preset: original PS1 picture", s.IsOriginal() ? "(active)" : ""});
        r.push_back({"Close", ""});
        break;
    case kResolution: {
        char res[32];
        if (s.renderHeight > 0) std::snprintf(res, sizeof(res), "%s", s.renderHeight == 2160 ? "4K (2160p)" : (std::to_string(s.renderHeight) + "p").c_str());
        else std::snprintf(res, sizeof(res), "%d%% of window", s.renderScale);
        r.push_back({"Render resolution", res});
        r.push_back({"Display", s.fullscreen ? "Fullscreen (borderless)" : "Window"});
        r.push_back({"Aspect", s.wide ? "Widescreen" : "4:3 with bars"});
        r.push_back({"VSync", OnOff(s.vsync)});
        r.push_back({"FPS cap", s.fpsCap > 0 ? std::to_string(s.fpsCap) : "Off"});
        r.push_back({"Back", ""});
        break;
    }
    case kImage:
        r.push_back({"Anti-aliasing (MSAA)", s.msaa > 1 ? std::to_string(s.msaa) + "x" : "Off"});
        r.push_back({"Texture filtering", s.smoothTextures ? "Smooth + mipmaps" : "PS1 nearest"});
        r.push_back({"Colour", s.ps1Colour ? "15-bit (PS1)" : "24-bit"});
        r.push_back({"Dither (15-bit colour)", OnOff(s.ps1Dither)});
        r.push_back({"Vertices", s.preciseVertices ? "Precise" : "PS1 GTE points (jitter)"});
        r.push_back({"Texture mapping", s.affine ? "PS1 affine" : "Perspective-correct"});
        r.push_back({"Draw order", s.ps1DrawOrder ? "PS1 ordering table" : "Depth buffer"});
        // HD media (hd_media.h, docs/HD-MEDIA.md): the pack's pictures, fonts, HUD and films; a note when none is installed
        r.push_back({"HD textures and media", std::string(OnOff(s.hdMedia)) + (rr::hd::LoadedPack() ? "" : " (no HD pack installed)")});
        r.push_back({"Back", ""});
        break;
    case kDetail: {
        static const char* const kDist[3] = {"Original (the game's draw list)", "Extended (streamed cells)",
                                             "Maximum (every loaded cell)"};
        r.push_back({"Maximum detail (no LOD, no pop-in)", OnOff(s.maxDetail)});
        r.push_back({"Draw distance", kDist[std::clamp(s.drawDistance, 0, 2)]});
        r.push_back({"Back", ""});
        break;
    }
    case kControls: {
        const rr::platform::Bindings& b = rr::platform::ActiveBindings();
        for (int a = 0; a < rr::platform::kGameActions; ++a) {
            const auto act = static_cast<rr::platform::GameAction>(a);
            std::string v = capturing == a ? "press a button / key..." :
                            rr::platform::BindingLabel(b, act, true) + "  |  " + rr::platform::BindingLabel(b, act, false);
            r.push_back({rr::platform::Action(act).label, v});
        }
        r.push_back({"DualSense adaptive triggers", OnOff(b.adaptiveTriggers)});
        for (const HandlingRow& h : HandlingMenuRows(false)) r.push_back({h.label, h.value});
        r.push_back({"Reset controls to defaults", ""});
        r.push_back({"Back", ""});
        break;
    }
    case kCheats:
        for (const CheatRow& c : CheatMenuRows()) r.push_back({c.label, c.value});
        r.push_back({"Back", ""});
        break;
    default:
        break;
    }
    (void)inRace;
    return r;
}

static const char* TitleOf(int page) {
    switch (page) {
    case kResolution: return "SETTINGS / RESOLUTION AND DISPLAY";
    case kImage: return "SETTINGS / IMAGE QUALITY";
    case kDetail: return "SETTINGS / DETAIL";
    case kControls: return "SETTINGS / CONTROLS  (gamepad | keyboard)";
    case kCheats: return "SETTINGS / CHEATS  (all off by default)";
    default: return "ROAD RASH: JAILBREAK PC - SETTINGS";
    }
}

void PcOverlay::Save() {
    if (SaveGraphics(SettingsPath(), Graphics())) status_ = "Saved: " + SettingsPath();
    else status_ = "Could not write " + SettingsPath();
}

void PcOverlay::Back() {
    if (page_ == kMain) {
        open_ = false;
        return;
    }
    selected_ = page_ - 1; // the main page's row of this page
    page_ = kMain;
    scroll_ = 0;
}

void PcOverlay::Activate(int direction) {
    GraphicsSettings& s = Graphics();
    const std::vector<Row> rows = RowsOf(page_, inRace_, capturing_);
    if (selected_ < 0 || selected_ >= static_cast<int>(rows.size())) return;
    const int d = direction == 0 ? 1 : direction;
    bool touched = true;
    switch (page_) {
    case kMain:
        if (selected_ <= 4) {
            if (direction == 0) {
                page_ = selected_ + 1;
                selected_ = 0;
                scroll_ = 0;
            }
            touched = false;
        } else if (selected_ == 5) {
            s.profiler = !s.profiler;
        } else if (selected_ == 6 && direction == 0) {
            const bool profiler = s.profiler;
            s = GraphicsSettings::Modern();
            s.profiler = profiler;
        } else if (selected_ == 7 && direction == 0) {
            const bool profiler = s.profiler;
            s = GraphicsSettings::Original();
            s.profiler = profiler;
        } else if (selected_ == 8 && direction == 0) {
            open_ = false;
            touched = false;
        } else {
            touched = false;
        }
        break;
    case kResolution:
        switch (selected_) {
        case 0: {
            const int cur = s.renderHeight > 0 ? -s.renderHeight : s.renderScale;
            const int next = Cycle(cur, kResolutions, d);
            if (next < 0) s.renderHeight = -next;
            else s.renderHeight = 0, s.renderScale = next;
            break;
        }
        case 1: s.fullscreen = !s.fullscreen; break;
        case 2: s.wide = !s.wide; break;
        case 3: s.vsync = !s.vsync; break;
        case 4: s.fpsCap = Cycle(s.fpsCap, kFpsCaps, d); break;
        default:
            touched = false;
            if (direction == 0) Back();
        }
        break;
    case kImage:
        switch (selected_) {
        case 0: s.msaa = Cycle(s.msaa, kMsaa, d); break;
        case 1: s.smoothTextures = !s.smoothTextures; break;
        case 2: s.ps1Colour = !s.ps1Colour; break;
        case 3: s.ps1Dither = !s.ps1Dither; break;
        case 4: s.preciseVertices = !s.preciseVertices; break;
        case 5: s.affine = !s.affine; break;
        case 6: s.ps1DrawOrder = !s.ps1DrawOrder; break;
        case 7: s.hdMedia = !s.hdMedia; break;
        default:
            touched = false;
            if (direction == 0) Back();
        }
        break;
    case kDetail:
        switch (selected_) {
        case 0: s.maxDetail = !s.maxDetail; break;
        case 1: s.drawDistance = (s.drawDistance + d + 3) % 3; break;
        default:
            touched = false;
            if (direction == 0) Back();
        }
        break;
    case kControls: {
        touched = false;
        rr::platform::Bindings& b = rr::platform::ActiveBindings();
        const int n = rr::platform::kGameActions;
        const int handlingRows = static_cast<int>(HandlingMenuRows(false).size());
        if (selected_ < n) {
            if (direction == 0) capturing_ = selected_; // the device that confirmed decides (Update)
        } else if (selected_ == n) {
            b.adaptiveTriggers = !b.adaptiveTriggers;
            rr::platform::SaveBindings(rr::platform::BindingsPath(), b);
            status_ = "Saved: " + rr::platform::BindingsPath();
        } else if (selected_ > n && selected_ <= n + handlingRows) { // the handling: saves itself
            status_.clear();
            HandlingMenuActivate(selected_ - n - 1, direction, false, status_);
        } else if (selected_ == n + handlingRows + 1 && direction == 0) {
            rr::platform::ResetToDefaults(b);
            rr::platform::SaveBindings(rr::platform::BindingsPath(), b);
            status_ = "Controls reset to the defaults";
        } else if (selected_ == n + handlingRows + 2 && direction == 0) {
            Back();
        }
        break;
    }
    case kCheats: // cheat_menu.h saves the [cheats] section itself
        touched = false;
        if (selected_ < static_cast<int>(rows.size()) - 1) {
            status_.clear();
            CheatMenuActivate(selected_, direction, status_);
        } else if (direction == 0) {
            Back();
        }
        break;
    default:
        touched = false;
    }
    if (touched) {
        changed_ = true;
        Save();
    }
}

bool PcOverlay::Update(const bool* keys, const rr::platform::GamepadState& pad) {
    const auto edge = [&](int vk) { return keys[vk] && !keyWas_[vk]; };
    using rr::platform::PadButton;
    const rr::platform::PhysicalPad& p = pad.raw;
    const bool padNow[8] = {p.Down(PadButton::DUp) || p.Down(PadButton::LsUp), p.Down(PadButton::DDown) || p.Down(PadButton::LsDown),
                            p.Down(PadButton::DLeft) || p.Down(PadButton::LsLeft), p.Down(PadButton::DRight) || p.Down(PadButton::LsRight),
                            p.Down(PadButton::Cross), p.Down(PadButton::Triangle), false, false};
    const auto padEdge = [&](int i) { return padNow[i] && !padWas_[i]; };
    const bool combo = p.Down(PadButton::Select) && p.Down(PadButton::Start);
    const bool toggle = edge(VK_F10) || (combo && !comboWas_);

    if (capturing_ >= 0) { // the Controls page: rebinding one action
        rr::platform::Bindings& b = rr::platform::ActiveBindings();
        static rr::platform::BindingCapture capture;
        if (!capture.Active()) capture.Begin(static_cast<rr::platform::GameAction>(capturing_), captureGamepad_, 0);
        const auto result = capture.Feed(b, captureGamepad_ ? &p : nullptr, captureGamepad_ ? nullptr : keys);
        if (result == rr::platform::BindingCapture::Result::Captured) {
            rr::platform::SaveBindings(rr::platform::BindingsPath(), b);
            status_ = "Saved: " + rr::platform::BindingsPath();
            capturing_ = -1;
        } else if (result == rr::platform::BindingCapture::Result::Cancelled) {
            status_ = "Rebinding cancelled";
            capturing_ = -1;
        }
    } else if (toggle) {
        open_ = !open_;
        page_ = kMain;
        selected_ = 0;
        status_.clear();
    } else if (open_) {
        const int rows = static_cast<int>(RowsOf(page_, inRace_, capturing_).size());
        const bool up = edge(VK_UP) || padEdge(0), down = edge(VK_DOWN) || padEdge(1);
        int horizontal = (edge(VK_LEFT) || padEdge(2)) ? -1 : ((edge(VK_RIGHT) || padEdge(3)) ? 1 : 0);
        // held Left / Right repeat (the render resolution list is long)
        const bool leftHeld = keys[VK_LEFT] || padNow[2], rightHeld = keys[VK_RIGHT] || padNow[3];
        const int held = leftHeld ? -1 : (rightHeld ? 1 : 0);
        if (horizontal != 0) {
            repeatDir_ = horizontal;
            repeatAt_ = Now() + 0.45;
        } else if (held != 0 && held == repeatDir_ && Now() >= repeatAt_) {
            horizontal = held;
            repeatAt_ = Now() + 0.12;
        } else if (held == 0) {
            repeatDir_ = 0;
        }
        const bool confirmKey = edge(VK_RETURN) || edge(VK_SPACE), confirmPad = padEdge(4);
        const bool back = edge(VK_ESCAPE) || edge(VK_BACK) || padEdge(5);
        if (up) selected_ = (selected_ + rows - 1) % rows;
        if (down) selected_ = (selected_ + 1) % rows;
        if (horizontal != 0) Activate(horizontal);
        if (confirmKey || confirmPad) {
            captureGamepad_ = confirmPad;
            Activate(0);
        }
        if (back) Back();
    }
    for (int k = 0; k < 256; ++k) keyWas_[k] = keys[k];
    for (int k = 0; k < 8; ++k) padWas_[k] = padNow[k];
    comboWas_ = combo;
    return open_;
}

void DrawProfiler(rr::render::TextOverlay& text, int w, int h, const ProfileNumbers& n) {
    const float scale = std::clamp(static_cast<float>(h) / 900.0f, 0.7f, 2.0f);
    std::vector<std::string> lines;
    char b[200];
    if (n.frames != nullptr) {
        std::snprintf(b, sizeof(b), "FPS %.1f  (1%% low %.1f)", n.frames->fps, n.frames->lowFps);
        lines.push_back(b);
        std::snprintf(b, sizeof(b), "frame %.2f ms  (worst %.1f)", n.frames->frameMs, n.frames->maxMs);
        lines.push_back(b);
    }
    std::snprintf(b, sizeof(b), "CPU %.2f ms  (draw %.2f)", n.cpuMs, n.renderCpuMs);
    lines.push_back(b);
    if (n.gpuMs >= 0) std::snprintf(b, sizeof(b), "GPU %.2f ms", n.gpuMs);
    else std::snprintf(b, sizeof(b), "GPU time: not available");
    lines.push_back(b);
    std::snprintf(b, sizeof(b), "draw calls %llu  triangles %llu", n.drawCalls, n.triangles);
    lines.push_back(b);
    std::snprintf(b, sizeof(b), "render %dx%d  MSAA %s", n.renderW, n.renderH,
                  n.samples > 1 ? (std::to_string(n.samples) + "x").c_str() : "off");
    lines.push_back(b);
    if (n.race) {
        std::snprintf(b, sizeof(b), "cells %zu drawn / %zu culled", n.cellsDrawn, n.cellsCulled);
        lines.push_back(b);
        std::snprintf(b, sizeof(b), "props %zu / %zu  objects %zu / %zu", n.propsDrawn, n.propsCulled, n.objectsDrawn,
                      n.objectsCulled);
        lines.push_back(b);
    }
    float width = 0;
    for (const std::string& l : lines) width = std::max(width, text.TextWidth(l, scale));
    const float pad = 8 * scale, lh = text.LineHeight(scale);
    text.Begin(w, h);
    text.Rect(0, 0, width + 2 * pad, lh * static_cast<float>(lines.size()) + 2 * pad, 0.0f, 0.0f, 0.0f, 0.62f);
    for (size_t i = 0; i < lines.size(); ++i)
        text.Text(pad, pad + lh * static_cast<float>(i), lines[i], 0.55f, 1.0f, 0.55f, 1.0f, scale);
    text.End();
}

void PcOverlay::Draw(rr::render::TextOverlay& text, int w, int h, const ProfileNumbers& numbers) {
    if (!text.Ready()) return;
    if (Graphics().profiler) DrawProfiler(text, w, h, numbers);
    if (!open_) return;
    const float scale = std::clamp(static_cast<float>(h) / 800.0f, 0.75f, 2.0f);
    const std::vector<Row> rows = RowsOf(page_, inRace_, capturing_);
    const float lh = text.LineHeight(scale) * 1.15f, pad = 16 * scale;
    const int visible = std::max(4, static_cast<int>((static_cast<float>(h) - 6 * lh) / lh));
    if (selected_ < scroll_) scroll_ = selected_;
    if (selected_ >= scroll_ + visible) scroll_ = selected_ - visible + 1;
    const int shown = std::min(visible, static_cast<int>(rows.size()) - scroll_);
    float labelW = 0, valueW = 0;
    for (const Row& r : rows) {
        labelW = std::max(labelW, text.TextWidth(r.label + (r.submenu ? "  >" : ""), scale));
        valueW = std::max(valueW, text.TextWidth(r.value, scale));
    }
    const std::string hint = "Up/Down select   Left/Right change   Enter/Cross open   Esc/Triangle back   F10 close";
    const float panelW = std::min(static_cast<float>(w) - 2 * pad,
                                  std::max({labelW + valueW + 5 * pad, text.TextWidth(TitleOf(page_), scale) + 2 * pad,
                                            text.TextWidth(hint, scale * 0.8f) + 2 * pad}));
    const float panelH = lh * static_cast<float>(shown + 4) + 2 * pad;
    const float x0 = (static_cast<float>(w) - panelW) * 0.5f, y0 = (static_cast<float>(h) - panelH) * 0.5f;
    text.Begin(w, h);
    text.Rect(0, 0, static_cast<float>(w), static_cast<float>(h), 0.0f, 0.0f, 0.0f, 0.35f);
    text.Rect(x0, y0, panelW, panelH, 0.06f, 0.07f, 0.09f, 0.92f);
    text.Rect(x0, y0, panelW, lh + pad, 0.55f, 0.12f, 0.08f, 0.95f);
    text.Text(x0 + pad, y0 + pad * 0.5f, TitleOf(page_), 1.0f, 0.95f, 0.85f, 1.0f, scale);
    float y = y0 + lh + pad * 1.5f;
    for (int i = scroll_; i < scroll_ + shown; ++i) {
        const Row& r = rows[static_cast<size_t>(i)];
        const bool sel = i == selected_;
        if (sel) text.Rect(x0 + pad * 0.5f, y - 2 * scale, panelW - pad, lh, 0.85f, 0.55f, 0.15f, 0.9f);
        const float c = sel ? 0.05f : 0.92f;
        text.Text(x0 + pad, y, r.label + (r.submenu ? "  >" : ""), c, c, c, 1.0f, scale);
        if (!r.value.empty()) {
            const std::string v = sel && !r.submenu ? "< " + r.value + " >" : r.value;
            text.Text(x0 + panelW - pad - text.TextWidth(v, scale), y, v, sel ? 0.05f : 1.0f, sel ? 0.05f : 0.8f,
                      sel ? 0.05f : 0.4f, 1.0f, scale);
        }
        y += lh;
    }
    y += lh * 0.4f;
    text.Text(x0 + pad, y, hint, 0.7f, 0.7f, 0.7f, 1.0f, scale * 0.8f);
    if (!status_.empty()) text.Text(x0 + pad, y + lh * 0.9f, status_, 0.6f, 0.9f, 0.6f, 1.0f, scale * 0.8f);
    text.End();
}

} // namespace rrgame
