#pragma once
// The VR settings menu - the F10 overlay's counterpart in the headset
// (pc_overlay.h), drawn on the theatre quad with the portable text overlay (render/text_overlay.h: GDI's Consolas on
// Windows, the built-in font on the Quest). The gt2-play project's VR menu is the model: opened with
// L3 + R3 (both stick clicks) or both grips + Menu, the game, its sound and the Touch haptics held while it is open;
// the left stick (or the right) up / down chooses a row, the TRIGGERS change it (left down, right up; the stick's
// left / right do not - a drifting stick would change values by itself), A opens / confirms, B goes back,
// Menu closes. On the desktop (the VR mock, PCVR's window) F10 opens it and the arrows / Enter / Esc drive it.
//
// Pages: the main page (resume, recentre, view, horizon lock, comfort, riding position, HUD size, vibration, quit),
// graphics and performance (eye resolution - applies at the next start -, refresh rate, MSAA, draw distance, detail,
// textures, foveation) and the controls (the Touch bindings, platform/input_bindings.h VrBindings, rebindable), with
// their Handlebar options (vr_bars_settings.h) and Combat options (vr_melee_settings.h) pages.
// "Weapons and holsters" (its first row "Calibrate the grip") and "Combat options" are rows of the main page too,
// after the values (Vibration) and before Graphics / Controls / Cheats; Back returns to where they were opened from.
// "Riding position" (main page row 6) gathers where the player sits and what the bike does under them: the seat height
// and forward / back, the eye, the bars' height, the drawn lean under Original, the bike pitch of the head view, and
// the [handling] rows of the view roll and the wheelies (the hand lift to start, the full-lift height, the hold to
// start, the angle, the view pitch, over cars, the loop-over).
#include "platform/input_bindings.h"
#include "render/text_overlay.h"
#include "vr_settings.h"

#include <string>
#include <vector>

namespace rrgame {

// One frame of the menu's input, as edges and held states (the VR host fills it from the Touch controllers and the
// keyboard).
struct VrMenuInput {
    bool up = false, down = false, left = false, right = false; // edges (with repeat for left / right)
    bool confirm = false, back = false, close = false;           // edges
    const rr::platform::PhysicalPad* pad = nullptr;              // the Touch controllers by position (rebinding)
};

// What a menu action asks the host to do.
struct VrMenuActions {
    bool recentre = false, quit = false, closed = false;
    bool refreshChanged = false, msaaChanged = false, foveationChanged = false, graphicsChanged = false;
    bool settingsChanged = false; // save the settings
};

class VrMenu {
public:
    void Open(int page = 0);
    void Close() { open_ = false; }
    bool IsOpen() const { return open_; }
    // One frame of input; returns what the host must do. `refreshRates`: the runtime's offered rates.
    VrMenuActions Update(const VrMenuInput& in, const std::vector<float>& refreshRates);
    // Draws the open menu over the bound framebuffer of w x h (the theatre quad's image).
    void Draw(rr::render::TextOverlay& text, int w, int h, const std::string& status) const;
    // "page/label: value" of the selected row (the host logs what changed a value); empty when closed.
    std::string SelectedRowText() const;

private:
    struct Row {
        std::string label, value;
        bool submenu = false;
    };
    std::vector<Row> Rows(const std::vector<float>& refreshRates) const;
    void Activate(int direction, const std::vector<float>& refreshRates, VrMenuActions& out);
    int page_ = 0, selected_ = 0;
    int weaponsFrom_ = 0, combatFrom_ = 0; // the page (main / Controls) Weapons and Combat were opened from
    bool open_ = false;
    int capturing_ = -1; // the controls page: the action being rebound
    rr::platform::BindingCapture capture_;
    std::string note_;
    std::vector<float> rates_;
};

} // namespace rrgame
