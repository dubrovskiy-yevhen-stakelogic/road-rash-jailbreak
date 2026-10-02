#pragma once
// The race's own results scene - what the original shows over the finished race while game_state
// +0x00 is 6, before it returns to the shell: RASHCDG 0x800C5918 (the scene), 0x800C6358 (the top
// three by place), 0x800C63CC (the heading and the "press START" line), and per race type 0x800C64F4
// / 0x800C6634 / 0x800C84F8 (career and the three story missions), 0x800C7510 (Five-O), with their
// row painters 0x800C5B5C (column headings), 0x800C5C68 / 0x800C5DF0 (a rider's row, the player's row
// with the For / Against tally) and 0x800C83D8 (a time as mm:ss:cc). RASHCDG.BIN sha1
// cfe43a7786759f2cb9c57751cf99e84d1074782c; read from work\ghidra\decomp_race.
//
// PRESENTATION, ported as layout: the scene runs in the front end's window
// on a copy of the race arena the race left (handover.h raceRam), and every string the original hands
// SLUS 0x8002CB08 / 0x8002CC74 becomes a TextCall (shell_view.h) in GAMEFONT.PFN, the race's font,
// with GAMESTRG.LOC's strings. Its one piece of logic is 0x800C84C0 - the writer of player[k]+0x20
// (the result code) and +0x00 (the finish stamp) that rules.md 7.2 leaves open: it is called here,
// once per row the scene paints, exactly as the original calls it.
//
// The two-player types (race type bit 0x10: 0x800C79D4 side by side with the running score player[k]
// +0x16, 0x11: 0x800C6FF8 the cops-and-robbers verdict) are ported as
// layout like the rest, and so is Time Trial (0x800C67FC: the record line, then the session's Time
// Trial players sorted by time).
#include "game/shell/shell_view.h"

#include <cstdint>
#include <string>
#include <vector>

namespace rr::shell {

class RaceResults {
public:
    // A copy of the race arena as the race left it (2 MiB), and GAMESTRG.LOC's strings.
    void Begin(std::vector<uint8_t> raceRam, const std::vector<std::string>* strings);
    bool Active() const { return active_; }
    bool Ran() const { return ran_; }
    void End() { active_ = false; }
    // One frame of RASHCDG 0x800C5918: the two colour cycles advance, the rows are painted into
    // `out` (font 4 = GAMEFONT). Returns false when the race type's arm is not ported.
    bool Frame(std::vector<TextCall>& out);
    // RASHCDG 0x800C84C0's effect on the player records (session+0x100.., 0x800D81D8 + 36k), read
    // back out of the copy for the handover: +0x20 and +0x00 of player k.
    uint32_t PlayerWord(int k, uint32_t offset) const;
    const std::string& Gap() const { return gap_; }

private:
    std::vector<uint8_t> ram_;
    const std::vector<std::string>* strings_ = nullptr;
    bool active_ = false;
    bool ran_ = false;
    std::string gap_;
};

} // namespace rr::shell
