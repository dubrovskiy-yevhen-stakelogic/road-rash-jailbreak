# Optional HD media

Road Rash: Jailbreak PC & VR can use an **HD pack** made on your own PC from your own disc: the front end's pictures
(backgrounds, titles, the animated panels, loading screens, the menu sprites), its fonts, the race HUD and the eleven
films (EA logo, intro, the career cut-scenes, credits), each four times larger. The pack is never downloaded and never
shipped: `PREPARE-HD.bat` makes it once, offline, and the game only reads it. Nothing is enlarged while playing.

## Making the pack

1. Install the game with your disc (`INSTALL-PC.bat`, `INSTALL-PCVR.bat` or `INSTALL.bat`). The installers offer the HD
   preparation at the end; you can also run it later.
2. Double-click **PREPARE-HD.bat** and choose:
   * **1** - pictures, fonts and HUD (a few minutes on a current GPU),
   * **2** - also the films (5347 film pictures; about half an hour on a fast GPU, more on others; a few GB of
     temporary files),
   * **3** - nothing.
3. Switch **HD textures and media** on: desktop **F10 > Image quality**, VR **menu > Graphics**. It is on by default
   for an interactive game; the switch keeps the pack installed and simply draws the originals when off.

Command line (the installed game, or any folder of your own):

```powershell
.\scripts\prepare-hd.ps1 -HdMedia MenusAndMovies                   # the installed game (install-location.txt)
.\scripts\prepare-hd.ps1 -Runtime 'D:\Games\RoadRashJailbreak' -HdMedia Menus -Upscaler 'D:\Tools\realesrgan'
.\scripts\install.ps1 -DiscImage 'D:\Discs\RRJB.cue' -HdMedia Menus  # install and prepare in one go
```

`-Films INTRO,EA_LOGO` limits the films, `-Gpu N` picks the Vulkan GPU, `-Tile 128` helps a GPU with little memory.
The films are streamed through the upscaler `-FilmChunk` (96) pictures at a time and the network's full-size output is
deleted after every chunk; `-Intermediate jpg` quarters the temporary space. The script checks the free space before
it starts and before every chunk (about 5 GB for pictures and films; `-MinFreeGB` overrides) and stops rather than fill
the drive; the finished pack is about 1.1 GB. `rrhd verify <disc> <pack> --deep` decodes every file of a pack.
Intermediate files stay in `runtime\.hd-work` so a stopped preparation resumes; delete that folder to reclaim the space.
A new pack replaces the old one only after it has been verified against your disc; the old one is kept as
`runtime\hd.backup-<date>`. Your disc copy and your saves are never changed.

### The neural upscaler (install it yourself)

The pictures and films are enlarged by **Real-ESRGAN** (`realesrgan-x4plus`, ncnn / Vulkan), the same tool and model
as the Gran Turismo 2 PC & VR port. The scripts do **not** download it. They use, in this order:

1. `-Upscaler <folder or exe>`;
2. the official release archive **realesrgan-ncnn-vulkan-20220424-windows.zip** from
   [Real-ESRGAN v0.2.5.0](https://github.com/xinntao/Real-ESRGAN/releases/tag/v0.2.5.0)
   (SHA-256 `ABC02804E17982A3BE33675E4D471E91EA374E65B70167ABC09E31ACB412802D`), found in `runtime\.hd-work`, in
   `Downloads`, or in the GT2 port's cache (`%LOCALAPPDATA%\GT2-VR\runtime\.hd-work`) - checked, then extracted;
3. an already extracted copy whose executable and model files match the archive's pinned hashes.

Without one of these the preparation stops and says exactly this. A Vulkan-capable GPU is required.

## What is prepared, and how

| Media | How | In the game |
|---|---|---|
| Front-end pictures: `DATA\FE\*.STR` frames (backgrounds, titles, 21 course/rider animations, mode panels), `*.TCM` loading screens, `FEMISC.PSH` sprites - 2770 pictures | Real-ESRGAN 4x. The sprites' transparency is the original's, enlarged edge-directed (Scale2x twice), never the network's | the menus, the loading screens and the results screen are drawn 4x larger (2048 x 960; 2x on the Quest) and filtered linearly with mipmaps |
| Fonts: `BTN_FONT`, `MINIFONT`, `HDR_FONT`, `GAMEFONT` | **contour atlases**: Hyllian's xBR-lv3 edge rules on the fonts' 4-bit indices, glyph by glyph (as the GT2 port's contour text) | each HD texel holds two original indices and a weight: the live text colour and the original semi-transparent edge rule still apply |
| The race HUD: every art rectangle and the HUD font of the HUD page, for one and two players | contour atlases per region, each with the CLUT it is drawn with | the HUD's own packets drawn 4x larger (2x on the Quest); a texel inside a matching region takes its contour texel **through the live CLUT**; flashing, fades and blending are the original's |
| Films: the eleven `.WVE` (320 x 192 / 224, 15 fps) | Real-ESRGAN 4x per picture, then **bounded**: each picture is pulled back to within 10 colour levels of a bicubic enlargement of its own original picture (as the GT2 port), and pixels whose picture has not changed by more than 3 levels keep the previous output (temporal stability, no shimmer on still backgrounds; cuts refresh at once) | the HD pictures replace the originals' at the film's own timing; the sound is the film's own |

Every entry of the pack is keyed by the **source hash** of the asset it was made from (FNV-1a 64 of the picture, font
sheet, HUD region with its CLUT, or film file, exactly as the game decodes it) and the pack by the **SHA-1 of your
`SLUS_010.53`**. The game compares both: a pack made from another disc is refused whole, and any entry whose source
differs is refused and the original is drawn. Anything the pack does not have is drawn from the disc as before.

**Limits.** A neural network cannot recover detail the originals do not have; it sharpens and can smooth fine texture.
The film bound trades some sharpness for fewer invented details. The contour fonts and HUD are smoothed outlines of
the original glyphs, not new typography. The race's world textures (road, buildings, bikes) are not part of the pack:
they already have **Texture filtering: Smooth + mipmaps** (F10 > Image quality), which is what the GT2 port does for
its world too.

## Files

`runtime\hd\` next to `rrgame.exe` (on the Quest `/sdcard/Android/data/com.rrjb.vr/files/hd`):
`profile.txt` (executable SHA-1), `index.txt` (kind, key, source hash, sizes, file per entry), `pictures\*.jpg|png`,
`fonts\*.png`, `hud\*.png`, `movies\*.rhm` (RRHDMOV1: one JPEG per film picture), `manifest.json` (SHA-256 of every
file). The format is described in `src\rrformats\hd_pack.h`. `RRJB_HD_PACK=<folder>` points the game elsewhere
(development).

## Quest

`scripts\install-quest-hd.ps1` copies the prepared pack to the headset (USB, developer mode), next to the disc copy;
the Quest installer does it by itself when a pack is prepared. `-Remove` takes it off again. The headset draws the menus
and the HUD 2x (its theatre screen and HUD panel are about 1000 pixels wide in the eye) and decodes each film picture on
a worker thread.
