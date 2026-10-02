// Two players on one screen: the parts of RaceSession that exist only for
// the original's split mode. The rest of the two-player path is in the session's own set-up and frame,
// each a one-line branch on `players_` next to the one-player code it extends (race_session.cpp).
//
// Images (our own disassembly): SLUS_010.53 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1,
// RASHCDI.BIN 9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06, RASHCDG.BIN cfe43a7786759f2cb9c57751cf99e84d1074782c.
#include "game/race_session.h"

#include "game/pad_product.h"
#include "game/shell/handover.h"
#include "game/sim/split_view.h"
#include "game/sim/stance.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace rr::game {
namespace {

using rr::sim::GuestRam;

constexpr uint32_t kGp = 0x8005AC8C;            // SLUS_010.53's gp (race_session.cpp kArenaGp)
constexpr uint32_t kGameStatePtr = 0x8005B2F8;
constexpr uint32_t kSession = 0x800D80D8;       // the front end's session record (handover.h)
constexpr uint32_t kPadRecord = 0x800D7128;     // the pad reader's frame-stable records, 192 bytes per player
constexpr uint32_t kPadRemap = 0x800526A4;      // SLUS: control -> slot, 60 bytes per configuration

} // namespace

// game_state+0x30 (the player count, which is also the road set) and +0x34 (the pads the
// pad reader loops over: 4 on the console after the shell's boot, OURS the players here); with two players
// the split layout: VIEWS.VI's 0x4C bytes where the boot's ViewsInit SLUS 0x8001BFA8 loads them (OURS: the
// race has no boot, so its disc door does the read) and the commit's ViewModeSet(session+0x17) (RASHCDF
// 0x8007F44C) - PORTED, split_view.h.
void RaceSession::MpSetupGame(const DiscImage& disc) {
    gameState_[0x30] = static_cast<uint8_t>(players_);
    gameState_[0x34] = static_cast<uint8_t>(players_);
    GuestRam g(arena_.Ram(), kGp);
    // RASHCDI 0x80063C00 (the race init 0x80063500 calls it at 0x800635A4), its two byte stores: the
    // results scene's "count this race" flag 0x8005AF55 = 1 (the two-player score, race_results.cpp) and
    // 0x8005AF54 = 0. Its third store, *(0x8005B598) = 0, is the frame's OT pointer the product keeps.
    g.W8(0x8005AF55u, 1);
    g.W8(0x8005AF54u, 0);
    if (players_ != 2) return;
    std::vector<uint8_t> views;
    if (const auto f = disc.Find("DATA/VIEWS.VI")) views = disc.ReadFile(*f);
    if (views.size() < rr::sim::kViewsFileBytes) {
        NoteSeam("two players: DATA\\VIEWS.VI is missing or short - there is no split layout");
        return;
    }
    g.WriteBlock(rr::sim::kViewsFile, views.data(), rr::sim::kViewsFileBytes);
    const rr::shell::Handover& fh = rr::shell::PendingHandover();
    uint32_t mode = fh.active ? g.U8(kSession + 0x17u) : 0u; // chooser 29 of the multiplayer options
    if (!fh.active && DevSplitMode() >= 0) mode = static_cast<uint32_t>(DevSplitMode()); // rrgame --split-mode
    if (mode > 2u) {
        NoteSeam("two players: the session's view mode " + std::to_string(mode) +
                 " names no VIEWS.VI record (0..2) - mode 0 is used");
        mode = 0;
    }
    rr::sim::ViewModeSet(g, mode);
    rr::sim::SplitRect a, b;
    rr::sim::SplitViewRect(g, 0, a);
    rr::sim::SplitViewRect(g, 1, b);
    char line[320];
    std::snprintf(line, sizeof(line),
                  "two players: game_state+0x30 = 2 (road set 2, STARTDFB.BIN), race type 0x%02X, "
                  "view mode %u (VIEWS.VI): player 1 at (%d,%d) %dx%d, player 2 at (%d,%d) %dx%d of the 384x240 area",
                  gameState_[0x04], mode, a.x, a.y, a.w, a.h, b.x, b.y, b.w, b.h);
    NoteSeam(line);
}

// Player 2's pad record, after the session cleared the records: its control table +0xB8 as the commit's
// SLUS 0x8001C4A8 installs it for player[1]+0x08 (the controller options' configuration; the product's
// player 1 keeps configuration 0, fight_session.cpp).
void RaceSession::MpSetupPads() {
    if (players_ != 2) return;
    GuestRam g(arena_.Ram(), kGp);
    const rr::shell::Handover& fh = rr::shell::PendingHandover();
    const uint32_t cfg = fh.active ? static_cast<uint32_t>(static_cast<int8_t>(g.U8(0x800D81D8u + 36u + 8u))) : 0u;
    g.W32(kPadRecord + 192u + 0xB8u, kPadRemap + 60u * (cfg < 8u ? cfg : 0u));
}

// Player 2's part of the pad handler (SLUS 0x8001CB3C's player loop, p = 1): the combat tail on record
// 0x800D71E8 (fight_session.cpp), the PORTED rider-controls region (pad_product.h, the axes 0x800CE548) and
// the camera controls on view record 1 (0x8001D700..0x8001D7D8, which the loop runs with s4 = 0x800CDD04).
void RaceSession::MpPlayerPad(const PadState& pad2, rr::sim::StanceSeams& seams) {
    if (players_ != 2 || bikes_.size() < 2) return;
    FightPadPass(pad2, seams, 1);
    TauntPad(seams, 1); // its L2 arm on record 1 / bike 1 (speech_session.cpp)
    const PadControlsResult r = RunPadControls(arena_.Ram(), kGp, bikes_[1].entityAddress, pad2.device, 1);
    if (!r.ok) {
        GuestRam g(arena_.Ram(), kGp);
        g.ClearFault();
        NoteSeam("the PORTED pad reader region SLUS 0x8001CFB0 faulted on player 2's record");
    }
    CameraControls(pad2, 1);
}

int RaceSession::SplitMode() const {
    if (players_ != 2) return -1;
    GuestRam g(const_cast<uint8_t*>(arena_.Ram()), kGp);
    return static_cast<int>(rr::sim::ViewMode(g));
}

bool RaceSession::SplitRect(int p, int16_t rect[4]) const {
    if (players_ != 2 || p < 0 || p > 1) return false;
    GuestRam g(const_cast<uint8_t*>(arena_.Ram()), kGp);
    rr::sim::SplitRect r;
    if (!rr::sim::SplitViewRect(g, static_cast<uint32_t>(p), r) || r.w <= 0 || r.h <= 0) return false;
    rect[0] = r.x;
    rect[1] = r.y;
    rect[2] = r.w;
    rect[3] = r.h;
    return true;
}

std::vector<uint32_t> RaceSession::HudListHeads() const {
    if (players_ == 2) return {ArenaWord(hudAt_.otP2), ArenaWord(hudAt_.ot)};
    return {ArenaWord(hudAt_.ot)};
}

} // namespace rr::game
