#include "render/window_win32.h"

#include "platform/png.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace rr::render {

WindowState g_window;

namespace {

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_CLOSE:
        case WM_DESTROY:
            g_window.quit = true;
            return 0;
        case WM_KEYDOWN:
            if (wparam < 256) g_window.key[wparam] = true;
            if (wparam == VK_ESCAPE && g_window.escapeQuits) g_window.quit = true;
            if (g_window.onKeyDown) g_window.onKeyDown(static_cast<int>(wparam));
            return 0;
        case WM_KEYUP:
            if (wparam < 256) g_window.key[wparam] = false;
            return 0;
        case WM_KILLFOCUS:
            // Keys held when the window loses focus never get their WM_KEYUP, so they would stay
            // stuck down. Clear them instead of leaving the throttle on.
            std::memset(g_window.key, 0, sizeof(g_window.key));
            return 0;
        default:
            return DefWindowProcA(hwnd, message, wparam, lparam);
    }
}

} // namespace

Window CreateGlWindow(int width, int height, const char* title, const char* className,
                      const char* focusEnvVar) {
    // One window per process: a second call (the race started from the front end, rrgame without
    // --race) gets the window and GL context the first one made.
    static Window existing;
    if (existing.hwnd != nullptr) {
        SetWindowTextA(existing.hwnd, title);
        wglMakeCurrent(existing.dc, existing.rc);
        return existing;
    }
    const HINSTANCE instance = GetModuleHandleA(nullptr);
    WNDCLASSA wc = {};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    wc.lpszClassName = className;
    RegisterClassA(&wc);

    RECT rect = {0, 0, width, height};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    Window w;
    w.hwnd = CreateWindowExA(0, className, title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance,
                             nullptr);
    if (!w.hwnd) throw std::runtime_error("CreateWindow failed");
    w.width = width;
    w.height = height;

    // How the window appears, in order of how much it gets in the way. A scripted run that only
    // wants a screenshot has no reason to cover the screen at all: not taking focus is not enough
    // when the window still sits on top of other work.
    //
    //   RRJB_WINDOW=hidden     never shown. The GL context still renders and glReadPixels still
    //                          reads the back buffer - verified by comparing a shot taken this way
    //                          with the same shot taken from a visible window, byte for byte
    //                          (SHA-256 210bf3de...e59e00 both times).
    //   <focusEnvVar> set      shown and focused (for playing).
    //   otherwise              shown without taking focus.
    //
    // MINIMIZED WAS TRIED AND REJECTED, twice over, and the measurements are why `hidden` exists:
    //   * a minimized window reports a 0x0 client rect every frame, so a loop that skips a
    //     degenerate rect never reaches its own screenshot-and-exit path - 626 seconds of CPU on a
    //     run that should take twenty. That spin is fixed separately (see `FrameSize`), because a
    //     user minimizing the game while playing must not burn a core either;
    //   * and even with that fixed, the driver does not render a minimized window's back buffer at
    //     all: the frame came out PURE BLACK, 1280x720 of it. A window mode that silently produces
    //     black screenshots is exactly the failure this project keeps having to catch, so it is not
    //     offered. `hidden` gets the window off the screen completely and is proven identical.
    //
    // NOT ShowWindow for the visible cases: the FIRST ShowWindow of a process takes its command from
    // the launcher's STARTUPINFO, so a request not to activate can be overridden from outside.
    const char* mode = std::getenv("RRJB_WINDOW");
    const bool hidden = mode != nullptr && std::strcmp(mode, "hidden") == 0;
    const bool wantFocus = !hidden && focusEnvVar != nullptr && std::getenv(focusEnvVar) != nullptr;
    const HWND had = GetForegroundWindow();
    if (hidden)
        ; // deliberately nothing: the window stays invisible for its whole life
    else if (wantFocus)
        ShowWindow(w.hwnd, SW_SHOW);
    else
        SetWindowPos(w.hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    // Say it rather than assume it: whether the window took the keyboard is checkable, and a
    // launcher or a future change to the window flags could silently turn it back on.
    const HWND now = GetForegroundWindow();
    char how[128];
    if (hidden)
        std::snprintf(how, sizeof(how), "NOT shown at all (RRJB_WINDOW=hidden)");
    else if (wantFocus)
        std::snprintf(how, sizeof(how), "shown with focus (%s is set)", focusEnvVar);
    else
        std::snprintf(how, sizeof(how), "shown WITHOUT taking focus");
    std::printf("window: %s (foreground %s)\n", how,
                now == w.hwnd ? "is this window" : (now == had ? "unchanged" : "changed to another window"));
    w.dc = GetDC(w.hwnd);

    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8; // the bike shadow's mask bit (race_scene.cpp DrawShadow)
    const int format = ChoosePixelFormat(w.dc, &pfd);
    if (!format || !SetPixelFormat(w.dc, format, &pfd)) throw std::runtime_error("no suitable pixel format");

    const HGLRC legacy = wglCreateContext(w.dc);
    if (!legacy || !wglMakeCurrent(w.dc, legacy)) throw std::runtime_error("wglCreateContext failed");

    auto createContextAttribs =
        reinterpret_cast<PFNWGLCREATECONTEXTATTRIBSARB>(wglGetProcAddress("wglCreateContextAttribsARB"));
    if (createContextAttribs) {
        const int attribs[] = {0x2091 /*MAJOR*/, 3, 0x2092 /*MINOR*/, 3, 0x9126 /*PROFILE_MASK*/, 0x0001 /*CORE*/, 0};
        if (const HGLRC core = createContextAttribs(w.dc, nullptr, attribs)) {
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(legacy);
            wglMakeCurrent(w.dc, core);
            w.rc = core;
        }
    }
    if (!w.rc) w.rc = legacy; // GL 3.3 unavailable: the shader path below will report it
    LoadGl();
    existing = w;
    return w;
}

bool HideIfScriptedRun(int argc, char** argv) {
    const char* mode = std::getenv("RRJB_WINDOW");
    if (mode != nullptr && std::strcmp(mode, "show") == 0) return false; // a deliberate look
    // A scripted run's stdout goes to a pipe or a file, where the CRT buffers all of it (a whole check
    // report fits in the buffer): a run that dies then leaves NOTHING behind. Unbuffered, what it printed
    // before it died is in the log (the gate flake with no output at all, run_gates.ps1 Gate).
    if (mode != nullptr && std::strcmp(mode, "hidden") == 0) {
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        return true; // already hidden
    }
    // Options that only a script uses: they bound the run, record it, or drive it without a person.
    static const char* const kScripted[] = {
        "--frames", "--shell-frames", "--shot", "--log", "--shell-log", "--shell-script", "--shell-race",
        "--hold", "--hold2", "--autosteer", "--autosteer2", "--sound-wav", "--dump-arena", "--log-from",
        "--start", "--sfx-probe", "--punch", "--punch-from", "--brake-from", "--chase", "--shadow",
        "--state", "--ticks", "--title-frames", "--pause-at", "--pause-script"};
    const char* why = nullptr;
    for (int i = 1; i < argc && why == nullptr; ++i) {
        const char* a = argv[i];
        if (a == nullptr || a[0] != '-' || a[1] != '-') continue;
        for (const char* s : kScripted)
            if (std::strcmp(a, s) == 0) { why = a; break; }
        // every development check option (--texcheck, --arenacheck, --hudcheck-mutate, ...)
        if (why == nullptr && std::strstr(a, "check") != nullptr) why = a;
    }
    if (why == nullptr) return false;
    _putenv_s("RRJB_WINDOW", "hidden");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("scripted run (%s): no visible window, no focus, no sound\n", why);
    return true;
}

bool PumpMessages() {
    MSG message;
    while (PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
    return !g_window.quit;
}

void FrameSize(const Window& window, int& width, int& height) {
    RECT client = {};
    if (window.hwnd != nullptr) GetClientRect(window.hwnd, &client);
    width = client.right - client.left;
    height = client.bottom - client.top;
    if (width <= 0 || height <= 0) {
        width = window.width;
        height = window.height;
    }
}

// ReadFrame / SaveShot: render/frame_shot.cpp (portable, shared with the VR and Quest builds).

} // namespace rr::render
