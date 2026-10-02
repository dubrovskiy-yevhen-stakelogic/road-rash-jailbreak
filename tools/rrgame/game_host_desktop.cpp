// The desktop host (game_host.h): the Win32 window and GL 3.3 context of render/window_win32.h, its keyboard, and
// the desktop controllers of platform/gamepad_win32.h - every call is the one RaceMain / GameMain made directly
// before the platform split, so the desktop game (and every gate's frame) is what it was.
#include "game_host.h"

#include "platform/gamepad_win32.h"
#include "render/render_target.h"
#include "render/window_win32.h"

#include <stdexcept>

namespace rrgame {

namespace {

class DesktopPad final : public PadDevice {
public:
    rr::platform::GamepadState Poll(int nth) override { return pad_.Poll(nth); }
    bool Vibrate(bool small, uint8_t large) override { return pad_.Vibrate(small, large); }
    void Triggers(bool racing) override { pad_.Triggers(racing); }

private:
    rr::platform::Gamepad pad_;
};

class DesktopHost final : public GameHost {
public:
    const char* Name() const override { return "desktop"; }
    void OpenWindow(int width, int height) override {
        window_ = rr::render::CreateGlWindow(width, height, "rrgame", "rrgame", "RRGAME_FOCUS");
    }
    bool Pump() override { return rr::render::PumpMessages(); }
    bool Closed() const override { return rr::render::g_window.quit; }
    const bool* Keys() const override { return rr::render::g_window.key; }
    void SetEscapeQuits(bool on) override { rr::render::g_window.escapeQuits = on; }
    void ClientRect(int& width, int& height) const override {
        RECT client;
        GetClientRect(window_.hwnd, &client);
        width = client.right - client.left;
        height = client.bottom - client.top;
    }
    void FrameSize(int& width, int& height) const override { rr::render::FrameSize(window_, width, height); }
    void Present() override { SwapBuffers(window_.dc); }
    void SetVsync(bool on) override { rr::render::SetSwapInterval(on); }
    void SetFullscreen(bool on) override { rr::render::SetBorderlessFullscreen(window_.hwnd, on); }
    std::unique_ptr<PadDevice> OpenPad() override { return std::make_unique<DesktopPad>(); }

private:
    rr::render::Window window_;
};

} // namespace

std::unique_ptr<GameHost> CreateDesktopHost() { return std::make_unique<DesktopHost>(); }

} // namespace rrgame
