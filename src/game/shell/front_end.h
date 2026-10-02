#pragma once
// The front end as the product runs it: the shell's frame loop RASHCDF
// 0x80080274 with its logic PORTED (shell_logic.h) on the shell arena (shell_arena.h), the pad
// snapshot filled from the keyboard, and the picture drawn by ShellView. Leaving it is the original's
// one byte: StartRaceInput 0x8006C354 writes game_state+0x00 = 3; the product then runs the ported
// commit 0x8007F37C and hands the race what the commit wrote (handover.h).
#include "game/pad_device.h" // the pad rumble
#include "game/shell/handover.h"
#include "game/shell/race_results.h"
#include "game/shell/movie_player.h"
#include "game/shell/panel_films.h"
#include "game/shell/shell_arena.h"
#include "game/shell/shell_memcard.h"
#include "game/shell/shell_view.h"
#include "rrvfs/disc_image.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rr::shell {

// The keys the shell reads, as the eight pad slots of the pad record.
struct ShellKeys {
    bool up = false, down = false, left = false, right = false;
    bool cross = false;    // confirm   - Enter
    bool triangle = false; // back      - Esc / Backspace
    bool square = false;   // options   - O
    bool circle = false;   // help      - H
};

// The product's side of every seam the ported shell reaches (shell_logic.h ShellCallees): the UI
// clicks and music requests are queued for the host to play; any other unported callee is recorded
// by address so the log says exactly what the frame asked for and did not get.
class ProductCallees final : public ShellCallees {
public:
    bool Call(uint32_t address, const uint32_t* args, int count, uint32_t* v0) override;
    void Drew(const DrawNote& n) override; // a ported widget handler's packet (shell_widgets.h), for the view
    std::vector<uint32_t> uiSounds;              // PlayUiSound(n) requests this frame
    int musicRequest = -1;                        // MusicPlay(track) this frame, -1 none
    bool musicStop = false;
    std::vector<std::pair<uint32_t, uint32_t>> unported; // (address, first argument), in order
    size_t totalUnported = 0;
    uint32_t rootCounter = 0; // OURS: the product's stand-in for root counter reads
    size_t standIns = 0;       // unported screen handlers answered by MenuInput (front_end.cpp)
    GuestRam* g = nullptr;     // the shell arena, for the stand-in
    // The text the ported text-block handler asked for this frame (shell_text.h), for the view.
    std::vector<TextCall> texts;
    uint32_t widget = 0;                 // the widget whose handler is running
    std::vector<uint32_t> textMissing;   // text arms not ported yet that were reached (each once)
    CardDevice card;                     // the card layer's seams over the card file (shell_memcard.h)
    PanelFilms* films = nullptr;         // the film library's and file layer's seams for the panel films (OURS)
    uint64_t filmGates = 0, logoCalls = 0; // PORTED 0x8006E4D8 / 0x8006E8FC calls (shell_panel.h), for the log
};

class FrontEnd {
public:
    FrontEnd(const DiscImage& disc, uint32_t seed);

    // One shell frame (RASHCDF 0x80080274's body).
    // `keys2`: player 2's keys into pad record 1 - the two-player screens' pad windows (screen 31: pad 1
    // only) read it.
    void Frame(const ShellKeys& keys, const ShellKeys* keys2 = nullptr);
    // The pad rumble: a controller that can vibrate (an XInput pad, not in a
    // scripted run) on pad 0 / 1. While either is there, each Frame runs the driver's actuator service SLUS
    // 0x8001DDC4 (PORTED) against the product's controller model (game\pad_device.h): a DualShock where one
    // is, else a first-type pad - so the port record's +0x04 = 1 shows the options' VIBRATION chooser (widget
    // condition 0x02000000), as a DualShock does on the console. Without one nothing runs (the captures' pad).
    void SetVibrationPads(bool pad0, bool pad1) {
        vib_[0] = pad0;
        vib_[1] = pad1;
    }
    // game_state+0x00 == 3: StartRaceInput has handed over.
    bool RaceRequested();
    // Runs the ported commit RASHCDF 0x8007F37C and fills `h` (handover.h) with what it wrote.
    void BeginRace(Handover& h);
    // Back from the race. First the race's own results scene (race_results.h) on the race's arena,
    // until Cross / Start - it writes player[0]+0x20 / +0x00 through the PORTED RASHCDG 0x800C84C0 -
    // then the session and player records into the shell, game_state+0x00 = 2 and the PORTED post-race
    // dispatch 0x8007FF4C.
    void EndRace(const Handover& h);
    bool ResultsShowing() const { return results_.Active(); }

    // HD media (docs\HD-MEDIA.md): the scale Draw draws at (1: the original 512 x 240; 2 or 4 with the active HD pack's
    // pictures, fonts and films - ShellView::SetScale, MoviePlayer::SetScale) and the size of what it draws.
    void SetHdScale(int scale) {
        view_->SetScale(scale);
        if (movie_) movie_->SetScale(view_->Scale());
        hdScale_ = view_->Scale();
    }
    int FrameWidth() const { return view_->Width(); }
    int FrameHeight() const { return view_->Height(); }

    void Draw(std::vector<uint8_t>& rgba) {
        if (results_.Active()) { // the race's results scene before the post-race dispatch (race_results.h)
            view_->DrawResults(*g_, resultsTexts_, rgba);
            return;
        }
        if (MovieDraw(rgba)) return; // a film fills the display while it plays (movie_player.h)
        view_->Draw(*g_, frames_, rgba, &callees_.texts);
    }
    // The films (movie_player.h). The sound is the host's to play: `start` once
    // when a film opens (its samples, valid until the next call), `stop` when it ends or is skipped.
    struct MovieAudio {
        bool start = false, stop = false;
        const std::vector<int16_t>* pcm = nullptr;
        int rate = 0, channels = 0;
    };
    MovieAudio TakeMovieAudio();
    bool MovieDraw(std::vector<uint8_t>& rgba) const;
    bool MoviePlaying() const { return movie_ && movie_->Playing(); }
    std::string TakeMovieNote() { std::string s; s.swap(movieNote_); return s; }
    GuestRam& Ram() { return *g_; }
    ShellArena& Arena() { return arena_; }
    ProductCallees& Callees() { return callees_; }
    uint32_t Frames() const { return frames_; }
    int CurrentScreen() { return g_->S16(kFeCur); }
    const ShellView& View() const { return *view_; }
    ShellView& MutableView() { return *view_; } // development: rrverify menuprims' blit log
    std::string Status();           // one log line: screen, selection, mode, seams
    std::string ArenaReport() const { return arenaReport_; }
    // The career card file (shell_card.h): the card screens 43..46 run PORTED (shell_memcard.h) and
    // their card layer is CardDevice over this file.
    void SetCardPath(const std::string& path) { cardPath_ = path; callees_.card.SetPath(path); }
    std::string TakeCardNote() {
        std::string s;
        s.swap(cardNote_);
        const std::string c = callees_.card.TakeNote();
        if (!c.empty()) s += (s.empty() ? "" : "; ") + c;
        return s;
    }

private:
    void WritePads(const ShellKeys& keys, uint32_t pad = 0);
    bool vib_[2] = {false, false};
    bool padsBooted_ = false;
    rr::game::PadDeviceModel padModel_;
    void ScreenTick();
    const DiscImage& disc_;
    ShellArena arena_;
    std::unique_ptr<GuestRam> g_;
    std::unique_ptr<ShellView> view_;
    ProductCallees callees_;
    uint32_t frames_ = 0;
    int held_[8] = {}, held2_[8] = {};
    std::string arenaReport_;
    std::string cardPath_, cardNote_;
    int lastScreen_ = -1;
    RaceResults results_;
    std::vector<TextCall> resultsTexts_;
    Handover pending_;
    bool prevCross_ = true;
    void FinishRace();
    // the films (front_end.cpp MovieEntry / MovieTick, the movie screens' transition and tick)
    void MovieEntry();
    void MovieTick();
    void MovieClose(bool skipped);
    std::unique_ptr<MoviePlayer> movie_;
    std::unique_ptr<PanelFilms> films_; // the panel films' library (panel_films.h)
public:
    const PanelFilms& Films() const { return *films_; }
private:
    int movieScreen_ = -1;
    int hdScale_ = 1; // SetHdScale
    MovieAudio movieAudio_;
    std::string movieNote_;
};

// The product entry point in front of the race (tools\rrgame): with no --race the front end runs
// first and each race it starts is `raceMain` with the handover's --race; any argument that asks for
// a check, a race or a scripted run goes straight to `raceMain`.
int GameMain(int argc, char** argv, int (*raceMain)(int, char**));

} // namespace rr::shell
