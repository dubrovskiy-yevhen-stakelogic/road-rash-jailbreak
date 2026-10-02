#pragma once
// rrgame's platform seam: the game's two loops - the race (main.cpp RaceMain) and
// the front end (front_end_main.cpp GameMain) - are portable code that draws with GL and reads its input through this
// interface; a HOST supplies the window or the headset, the keyboard, the controllers and the frame's presentation.
// Model: gt2-play's game_window.h WindowBackend (the gt2-play project, MIT), cut down to what this game's
// immediate-mode GL loops need.
//
// The hosts:
//   * desktop (game_host_desktop.cpp, Windows): the Win32 window of render/window_win32.h and the desktop controllers
//     (platform/gamepad_win32.h) - exactly what the two loops called directly before the split;
//   * VR (game_host_vr.cpp, Windows and Android): an OpenXR session (rrgame --vr on Windows, the Quest's NativeActivity
//     in main_android.cpp) or the desktop VR mock (rrgame --vr-mock: the same VR path into offscreen eyes, no headset),
//     the Touch controllers through the VR bindings, the VR settings menu, and on Windows a desktop window for the
//     GL context and the keyboard.
// The process has one host (SetHost at start-up; Host() everywhere else).
#include "platform/gamepad_state.h"

#include <cstdint>
#include <memory>

namespace rrgame {

class VrHost; // game_host_vr.h

// One controller as a loop reads it: a desktop Gamepad, or the Touch controllers in VR. One object per call site,
// as the loops held one Gamepad each before (their Poll keeps per-object state).
class PadDevice {
public:
    virtual ~PadDevice() = default;
    // The `nth` connected controller (0 = the first; a two-player race asks 1 for player 2).
    virtual rr::platform::GamepadState Poll(int nth = 0) = 0;
    // The DualShock's two motors (rumble_product.h); true when the device was written.
    virtual bool Vibrate(bool small, uint8_t large) = 0;
    // A DualSense's adaptive triggers while `racing` (nothing elsewhere).
    virtual void Triggers(bool racing) = 0;
};

class GameHost {
public:
    virtual ~GameHost() = default;
    virtual const char* Name() const = 0;
    // The GL context and its window, made the first time and reused afterwards (one per process, as
    // render/window_win32.h CreateGlWindow): the race and the front end both call it with the size they want.
    virtual void OpenWindow(int width, int height) = 0;
    // Services the window's / the runtime's events; false once the window was closed or the runtime asked to quit.
    virtual bool Pump() = 0;
    virtual bool Closed() const = 0;
    // The keyboard: 256 held flags indexed by Windows virtual-key code (platform/vk_codes.h); all false without one.
    virtual const bool* Keys() const = 0;
    // Esc closes the window (render/window_win32.h WindowState::escapeQuits); the loops turn it off.
    virtual void SetEscapeQuits(bool on) = 0;
    // The window's client area as it is (0 x 0 when minimized) and with the created size as the minimized fallback.
    virtual void ClientRect(int& width, int& height) const = 0;
    virtual void FrameSize(int& width, int& height) const = 0;
    // Shows the frame the loop drew into the window's framebuffer (SwapBuffers). The VR host presents its own frames.
    virtual void Present() = 0;
    virtual void SetVsync(bool on) = 0;
    virtual void SetFullscreen(bool on) = 0;
    virtual std::unique_ptr<PadDevice> OpenPad() = 0;
    // The VR host's own interface (stereo frames, the theatre quad, the VR menu); null on the desktop.
    virtual VrHost* Vr() { return nullptr; }
    // A long load (the race's world) between two frames: the VR host shows the last theatre picture again so the
    // compositor keeps getting frames. Nothing on the desktop.
    virtual void LoadingTick() {}
};

GameHost& Host();
void SetHost(std::unique_ptr<GameHost> host);
bool HostSet();

// The desktop host (game_host_desktop.cpp; Windows only).
std::unique_ptr<GameHost> CreateDesktopHost();

} // namespace rrgame
