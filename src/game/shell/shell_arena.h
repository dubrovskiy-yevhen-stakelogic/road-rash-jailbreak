#pragma once
// The shell's guest arena: one 2 MiB image of the console's RAM as the front end has it after its
// cold boot RASHCDF 0x8007FEDC, built from the player's disc - SLUS_010.53 at its load address, the shell
// overlay RASHCDF.BIN at 0x8005B5E8 - and then the PORTED initialisers of that boot run on it.
// Every ported shell function
// (shell_logic.h) runs on this image, at the original's addresses, exactly as the race runs on
// RaceSession's arena.
//
// `rrshell arenacheck <statedir>` compares the tables this builds with a capture of the
// running shell (work\oracle\state\retro-shell), byte for byte.
#include "game/shell/shell_logic.h"
#include "rrvfs/disc_image.h"

#include <cstdint>
#include <string>
#include <vector>

namespace rr::shell {

constexpr uint32_t kArenaGp = 0x8005AC8C; // gp of SLUS_010.53, as every capture holds it

struct ShellArena {
    std::vector<uint8_t> ram = std::vector<uint8_t>(GuestRam::kRamSize, 0);
    std::vector<uint8_t> scratchpad = std::vector<uint8_t>(1024, 0);
    GuestRam View() {
        GuestRam g(ram.data(), kArenaGp);
        g.SetScratchpad(scratchpad.data());
        return g;
    }
};

// Loads the two images and runs the boot's ported initialisers (up to, not including, the shell's
// frame loop). `seed` replaces the hardware root counter FrontendInit seeds the LCG from (ours: the
// console reads a timer the port does not have). The callees the boot reaches that the port does
// not contain go through `k` (the sound-mode switch of the chooser sync). Throws on a missing file.
void BuildShellArena(const DiscImage& disc, ShellArena& arena, ShellCallees& k, uint32_t seed, std::string* report);

// The ported initialisers, each by its original's address (also used by the bench and the check).
void GameStateInit(GuestRam& g);        // SLUS 0x80011738
void SessionInit(GuestRam& g);          // RASHCDF 0x8007FDA8
void VolumeSet(GuestRam& g, int32_t index, int32_t value); // RASHCDF 0x8007F20C
void ChooserTableInit(GuestRam& g);     // RASHCDF 0x80063720
void NavInit(GuestRam& g);              // RASHCDF 0x80068E88
void HandlerTableInit(GuestRam& g);     // RASHCDF 0x8006D174
void ScreenTableInit(GuestRam& g);      // RASHCDF 0x800806D0
void FrontendInit(GuestRam& g, uint32_t seed); // RASHCDF 0x800665E8
bool BootSelections(GuestRam& g, ShellCallees& k); // RASHCDF 0x80080C04 (0x80080C50, 0x80064254, 0x80063BFC)
// The text resources of 0x80080544: FESTRING.LOC relocated (0x80065F4C) and the three fonts' slots
// (the font arm of 0x80065CA8), at the capture's addresses. Throws on a missing file.
void LoadTextResources(const DiscImage& disc, GuestRam& g);

// The arena check against a capture of the running shell: every table the boot builds that nothing
// changes afterwards, compared byte for byte. Returns the number of differing bytes (0 = pass).
size_t CompareArenaWithCapture(const ShellArena& arena, const std::vector<uint8_t>& captureRam, std::string& report);

} // namespace rr::shell
