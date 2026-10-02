#pragma once
// rrgame's side of the optional HD media pack (docs\HD-MEDIA.md, src\rrformats\hd_pack.h): where the pack is, the
// "HD textures and media" switch, and the HD HUD's texture. The shell draws its HD frame itself (ShellView::SetScale).
//
// The pack folder: RRJB_HD_PACK when set, else <folder of rrgame.exe>\runtime\hd (the installed layout,
// scripts\prepare-hd.ps1 writes it), on the Quest <app external files>/hd (scripts\install-quest-player.ps1 -HdPack
// pushes it). A scripted run uses the pack only when its command line says --hd-media 1.
#include "game/hud_hd.h"
#include "render/gl_api.h"
#include "rrvfs/disc_image.h"

#include <string>
#include <vector>

namespace rrgame {

// Opens the pack for `disc` once a process (again for another disc) and prints one line: the folder and what it holds,
// or why there is none / why it was refused (another disc's pack).
void OpenHdPack(const rr::DiscImage& disc);
// The switch (graphics settings hd_media / the VR setting); the pack stays loaded while it is off.
void ApplyHdSwitch(bool on);
// The shell's draw scale: the pack's (rr::hd::DrawScale) while it is active, else 1.
int HdShellScale();

// The HD HUD (hud_hd.h): rasterises `packets` at the HD scale and uploads them into `texture` (resized, linear with
// mipmaps). False - nothing done - when HD media are off or there is no pack; the caller then takes the 1x path.
// `stampCheats` puts the CHEATS tag in (cheat_menu.h), enlarged.
bool UploadHudHd(rr::game::HudHd& hd, const std::vector<rr::game::HudPacket>& packets, const rr::game::HudVram& vram,
                 int32_t originX, int32_t originY, GLuint texture, bool stampCheats, int& uploadedW, int& uploadedH);

} // namespace rrgame
