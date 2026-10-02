#pragma once
// The in-race pause in the product: the PORTED pad poll
// pause test, GameFrame's stall switch and the pause menu (src\game\sim\pause.h) run by RaceSession::Frame
// (pause_product.cpp) on the session's arena, their sounds on the session's sound runtime.
//
//   Frame:  VSync clock -> the pad records' stamp (FightPadStamp) -> PausePoll (the PORTED region
//           0x8001CBEC..0x8001CD00, s0) -> the PORTED clock region with that s0 -> ... -> the rider-control
//           merge only while racing (the pad reader's `state == 1` test 0x8001CF8C) -> the stream step ->
//           PauseStall -> RaceStep (its +0x18 is 0 unless racing) -> ViewPass only while racing, AnimationPass
//           while racing or the countdown / post-race word 0x8005B230 is positive (GameFrame's two gates) ->
//           ... HudFrame -> PauseMenuFrame (state 3 / 4: PauseMenu 0x8002D2F4 into slots 6..2 of the frame's
//           2D ordering table *(0x8005B5AC), which the renderer walks after the HUD's).
//
// OURS, named: *(0x8005B5AC) = 0x800D9C80 (every capture's 12-slot table, whose slots 8 and 10 the HUD's
// placement already uses); the five slots are cleared to terminators each frame and walked one by one (the
// original clears and chains the whole table); the keyboard's roles (rrgame --keys: Enter / Esc / P = Start
// in the race; in the menu the arrows or W A S D = the d-pad, Enter / Space = Cross, Esc / Backspace =
// Triangle). RRJB_PAUSE=off is the negative control: the pause test answers "no pause" and none of it runs.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace rr::game {

bool PauseOn();

struct PadState; // race_session.h

// ---- OURS: the host's keys and a controller's buttons for the pause (rrgame; rrgame --keys).
// In the race: Enter, Esc, P or the controller's Start = Start (control 1, slot 12). Paused (state 3 / 4) the
// race pad is replaced by the menu pad: the arrows or W A S D = the d-pad, Enter / Space = Cross, Esc /
// Backspace = Triangle, P = Start; the controller's d-pad, Cross, Triangle, Start. A key keeps the role it
// had when it went down until it is released, so the Enter that paused is not also the menu's Cross.
struct PauseKeyInput {
    bool enter = false, escape = false, back = false, space = false, p = false;
    bool up = false, down = false, left = false, right = false; // arrows or W A S D
    bool padStart = false, padUp = false, padDown = false, padLeft = false, padRight = false, padCross = false,
         padTriangle = false;
};
class PauseKeys {
public:
    void Apply(PadState& pad, const PauseKeyInput& in, bool paused);

private:
    int enterRole_ = 0, escRole_ = 0; // 0 released, 1 Start, 2 Cross (Enter) / Triangle (Esc)
};

// A scripted run's presses (rrgame --pause-at N, --pause-script "f:key,key;f:key"; keys start up down left
// right x t), each held for the one frame named. Only the race's first attempt runs it (a restart does not).
class PauseScript {
public:
    void Parse(const std::string& s);
    void At(long frame, const char* key);
    bool Has(long frame, const char* key) const;
    bool Empty() const { return events_.empty(); }

private:
    std::vector<std::pair<long, std::string>> events_;
};
// rrgame's race loop: the race asked for itself again (the menu's RESTART, state 5, main 0x8001247C ->
// 0x800122FC); RaceMain returns kRaceRestart and its caller runs it again with the same arguments.
constexpr int kRaceRestart = 0x52535452; // "RSTR"
int& RaceAttempt(); // 0 for the first run of a race in this process, then 1, 2 ...

struct PauseCounts {
    uint64_t pauses = 0;          // the pause test took a racing game to state 3
    uint64_t pausedFrames = 0;    // frames the session spent in state 3 or 4
    uint64_t menuFrames = 0;      // PauseMenu calls
    uint64_t menuRefused = 0;     // ... that refused (a callee, a fault)
    uint64_t resumes = 0;         // state 3 -> 1
    uint64_t quits = 0;           // state 3 -> 6 (QUIT GAME, YES)
    uint64_t restarts = 0;        // state 3 -> 5 (RESTART GAME, YES)
    uint64_t stallIn = 0, stallOut = 0; // GameFrame's switch into / out of state 4
    uint64_t soundHolds = 0, soundHoldRefused = 0, overSignals = 0;
    uint64_t drawnPackets = 0;    // packets in the menu's slots on the paused frames (the renderer's walk)
    uint64_t resultsKeys = 0;     // pad presses in the results state (*(0x8005AF50))
};
PauseCounts& PauseCounters();
std::string PauseTotals();

// The frame comparison (rrgame --pausecheck <prims.csv>): the pause menu's GP0 commands the session's slots
// hold now, against the original's in a `rrverify trace` prims.csv (the last transfer holding the menu's box
// 0x800540E8, from that packet through the menu's last packet before the frame's VRAM copy): words compared
// in order (heap addresses differ by construction). `mutate`: the negative control, one colour word changed.
class RaceSession;
std::string PausePacketCheck(const RaceSession& s, const std::string& primsCsv, bool mutate, bool* pass);

} // namespace rr::game
