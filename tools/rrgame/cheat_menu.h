#pragma once
// The cheat menu (src\game\cheats.h): the page both menus show - the F10 overlay's "Cheats"
// (pc_overlay.h) and the VR menu's (vr_menu.h) - its [cheats] section of rrgame_settings.ini, the --cheat-* flags of a
// scripted run, the career cheats on the memory card and the HUD's CHEATS tag. The pattern of gt2-play's cheats
// (the gt2-play project, MIT): every cheat off by default, reversible, a scripted run never reads the file; a career
// write keeps a one-time backup of the card (<card>.before-cheats.bak), writes a candidate beside it, reads it back
// and checks every career record's checksum before it replaces the card.
#include <cstdint>
#include <string>
#include <vector>

#include "game/cheats.h"

namespace rr {
class DiscImage;
}

namespace rrgame {

// ---- settings
// Reads the [cheats] section into `s` (missing keys keep what `s` holds). False when the file cannot be read.
bool LoadCheats(const std::string& path, rr::game::CheatSettings& s);
// Rewrites the [cheats] section, keeping every other line of the file. False on a write error.
bool SaveCheats(const std::string& path, const rr::game::CheatSettings& s);
// A --cheat-* flag at argv[i] (advances i past its value); false when argv[i] is not one; a bad value throws
// std::runtime_error. --cheat-weapon W|off, --cheat-weapon-now F:W (the menu's "put it in the hand now" at frame F),
// --cheat-swings, --cheat-no-police, --cheat-no-traffic, --cheat-sparring passive|fight, --cheat-sparring-attack,
// --cheat-sparring-weapon own|fists|0..8, --cheat-sparring-standing, --cheat-passive,
// --cheat-god, --cheat-nitro, --cheat-freeze-timer, --cheat-at F:key=value (a [cheats] key set at frame F, as the menu
// would mid-race).
bool ApplyCheatFlag(int argc, char** argv, int& i);
// The cheats a run starts from: a scripted run (a frame count, a check, a hidden run) all off plus its flags; an
// interactive one the settings file (read once a process) plus the flags. Returns rr::game::Cheats().
rr::game::CheatSettings& StartCheats(bool scripted);
// --cheat-weapon-now / --cheat-at: their frame's request or change (rrgame's frame loop calls it every frame).
void CheatScriptFrame(long frame);
// The disc's weapon list for the menus (rr::game::SetCheatWeapons), once a process; a line for the log.
std::string LoadCheatWeapons(const rr::DiscImage& disc);

// ---- the page (both menus): rows, and Enter (0) / Left (-1) / Right (+1) on a row. `note` gets what to show.
struct CheatRow {
    std::string label, value;
};
std::vector<CheatRow> CheatMenuRows();
// True when a setting changed (it is saved to the settings file already).
bool CheatMenuActivate(int row, int direction, std::string& note);

// ---- the career on the memory card
void SetCheatCardPath(const std::string& path); // the front end's card (saves\rrjb_card.mcr)
enum class CareerCheat { MaxNitroWeapons, NextVenue };
// Applies `what` to the card's current career slot with the backup / candidate / verify / replace sequence. The
// RASHCDF tables it reads (the per-venue nitro and weapon-level caps) come from `disc`. Returns the status line;
// `ok` false when nothing was written.
std::string ApplyCareerCheat(const rr::DiscImage& disc, const std::string& cardPath, CareerCheat what, bool* ok = nullptr);
// The disc the menus' career rows use (the front end's / the race's), null until set.
void SetCheatDisc(const rr::DiscImage* disc);

// ---- the HUD's tag
bool CheatsShown(); // any cheat on
// "CHEATS" in the top middle of the HUD image (premultiplied RGBA, row 0 at the top) - the race's HUD on the desktop
// and on the VR panel.
void StampCheatsTag(uint8_t* rgba, int width, int height);

} // namespace rrgame
