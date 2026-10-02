#pragma once
// The one window each tool opens, and the GL 3.3 context on it.
//
// The window is created WITHOUT `WS_VISIBLE` and shown with `SetWindowPos(..., SWP_NOACTIVATE)`,
// so starting the game does not take the keyboard away from whatever else is running. Not
// `ShowWindow`: the FIRST `ShowWindow` of a process takes its command from the launcher's
// `STARTUPINFO`, so `SW_SHOWNOACTIVATE` can be turned back into "show and activate" by whoever
// started the run. That was established in the gt2-play project and is why the call is what it is.
//
// `CreateGlWindow` prints a `window:` line saying what actually happened, measured with
// `GetForegroundWindow()` before and after, so the claim is checkable and can report the bad
// outcome as well as the good one.
#include "render/gl_api.h"

#include <functional>
#include <string>
#include <vector>

namespace rr::render {

struct Window {
    HWND hwnd = nullptr;
    HDC dc = nullptr;
    HGLRC rc = nullptr;
    int width = 0;  // the size asked for, kept because a minimized window reports a 0x0 client rect
    int height = 0;
};

// The size to render at. Normally the client rect; when that is degenerate - which is what a
// MINIMIZED window reports, every frame - it falls back to the size the window was created with.
//
// This is not a nicety. A frame loop that simply skips a degenerate rect spins forever without ever
// reaching the code that takes its screenshot and exits: measured at 626 seconds of CPU on a run
// that should have taken twenty. A minimized window renders perfectly well to its back buffer, so
// the right answer is to keep drawing at the size we asked for, not to stop.
void FrameSize(const Window& window, int& width, int& height);

// The state the window procedure writes into. One window per process, so one record.
struct WindowState {
    bool quit = false;
    bool key[256] = {};           // held state, from WM_KEYDOWN / WM_KEYUP
    std::function<void(int)> onKeyDown; // edge events, for menus and one-shot toggles
    bool escapeQuits = true;      // the front end reads Esc as its back button instead (front_end.h)
};

extern WindowState g_window;

// Called first thing in main() by every tool that can open a window. A run is SCRIPTED when its
// command line bounds it or records it (--frames, --shell-frames, --shot, --log, a check option,
// held keys...) - nobody is sitting in front of it - and then this makes the whole process hidden and
// silent (it sets RRJB_WINDOW=hidden, which the window and the audio device both honour) no matter
// what the caller forgot to set. Only an open-ended run, like play.bat's, gets a visible window.
// RRJB_WINDOW=show on the environment overrides it for a deliberate look. Returns true if it hid.
bool HideIfScriptedRun(int argc, char** argv);

// `focusEnvVar` names the environment variable that asks for the old, focus-taking behaviour
// (`RRVIEW_FOCUS` / `RRGAME_FOCUS`); pass nullptr for "never take focus".
Window CreateGlWindow(int width, int height, const char* title, const char* className,
                      const char* focusEnvVar);

// Drains the message queue. Returns false once the window has been asked to close.
bool PumpMessages();

} // namespace rr::render

// ReadFrame (the frame as the GPU has it, bottom row first - call it BEFORE SwapBuffers) and SaveShot (a PNG of it).
#include "render/frame_shot.h"
