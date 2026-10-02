#pragma once
// The PC graphics settings of rrgame: what the F10 overlay edits, what
// `rrgame_settings.ini` next to rrgame.exe keeps between runs, and what the command line sets for a scripted run.
//
// Presentation only. Nothing of the simulation reads them: the 30 Hz / tick logic, the arena, the bench and the
// logs are the same whatever they say. Two presets:
//   * Original() - the frame exactly as rrgame drew it before the settings existed: the window's own framebuffer,
//     no MSAA, PS1 nearest texels, the GTE's integer screen points, affine texture mapping and the original's
//     subdivision, the ordering-table draw order, 15-bit colour with the dither, the original's levels of detail,
//     draw lists and draw ranges, 4:3 with bars. Every scripted run (a frame count, a shot, a check, --parity)
//     starts from it and never reads the settings file, so the gates see today's frames.
//   * Modern() - a normal interactive start with no settings file: render scale 100 %, MSAA 4x, smooth textures
//     with mipmaps, precise (float) vertices, perspective-correct mapping, the depth buffer, 24-bit colour,
//     maximum detail at the extended draw distance, widescreen, VSync on.
// The command line (ApplyFlag) sets single values on top of whichever preset the run starts from.
#include <string>

namespace rrgame {

struct GraphicsSettings {
    // ---- resolution
    int renderScale = 100;   // percent of the window, 50..200 (when renderHeight == 0)
    int renderHeight = 0;    // a fixed internal height 720 / 1080 / 1440 / 2160 (width from the window's aspect), 0 = scale
    bool fullscreen = false; // borderless over the monitor the window is on
    bool wide = false;       // the whole window, more at the sides (F2); false: the original's 4:3 with bars
    bool vsync = true;
    int fpsCap = 0;          // frames a second, 0 = none
    // ---- image
    int msaa = 1;               // 1 (off), 2, 4, 8
    bool smoothTextures = false; // bilinear + trilinear mipmaps; false: the PS1's nearest texel
    bool ps1Dither = true;       // F3
    bool ps1Colour = true;       // F4: 15-bit colour; false: 24-bit
    bool preciseVertices = false; // float vertices; false: the GTE's integer screen points (the PS1 jitter)
    bool affine = true;           // the PS1's affine texture mapping and the original's subdivision
    bool ps1DrawOrder = true;     // the ordering-table order; false: the depth buffer
    // ---- detail
    bool maxDetail = false; // every model at LOD 0, every cell group fine, the road's lane lines at every distance
    int drawDistance = 0;   // 0 original (the draw list), 1 extended (the residency windows), 2 maximum (all loaded cells)
    // ---- media
    bool hdMedia = false;   // "HD textures and media": the HD pack's pictures, fonts, HUD and films (docs/HD-MEDIA.md)
    // ---- tools
    bool profiler = false;

    static GraphicsSettings Original();
    static GraphicsSettings Modern();
    bool operator==(const GraphicsSettings&) const = default;

    // key=value of the settings file / the command line (false: unknown key or bad value)
    bool Parse(const std::string& key, const std::string& value);
    std::string Serialize() const; // the [graphics] section's lines
    std::string Describe() const;  // one log line
    // whether the picture needs the offscreen target (scale, fixed height or MSAA)
    bool NeedsTarget() const { return renderScale != 100 || renderHeight != 0 || msaa > 1; }
    bool IsOriginal() const { return *this == Original(); }
};

// The settings of this process (rrgame's race and front end share them).
GraphicsSettings& Graphics();
// `saves\rrgame_settings.ini` next to rrgame.exe (an older one beside the exe is copied there once);
// RRJB_SETTINGS overrides the path.
std::string SettingsPath();
// Reads the [graphics] section into `s` (missing keys keep what `s` holds). False when the file cannot be read.
bool LoadGraphics(const std::string& path, GraphicsSettings& s);
// Rewrites the [graphics] section, keeping every other line of the file. False on a write error.
bool SaveGraphics(const std::string& path, const GraphicsSettings& s);

// A graphics flag of the command line at argv[i] (advances i past its value). False when argv[i] is not one; a bad
// value throws std::runtime_error. The flags: --gfx original|modern, --render-scale P, --render-height H,
// --fullscreen 0|1, --vsync 0|1, --fps-cap N, --msaa N, --textures nearest|smooth, --precise-vertices 0|1,
// --affine 0|1, --draw-order ps1|depth, --max-detail 0|1, --draw-distance original|extended|maximum,
// --profiler 0|1, --hd-media 0|1. (--wide, --no-dither and --smooth stay RaceMain's own.)
bool ApplyGraphicsFlag(int argc, char** argv, int& i, GraphicsSettings& s);
// The settings a run starts from: a scripted run (a frame count, a check, --parity, a hidden run) Original() and the
// command line's graphics flags; an interactive one the settings file (read once a process; Modern() when there is
// none) and the flags on top. Returns Graphics().
GraphicsSettings& StartGraphics(bool scripted);
// Whether the command line set `key` (the settings file's name, e.g. "vsync"): a scripted run applies VSync only then.
bool GraphicsFlagGiven(const std::string& key);

} // namespace rrgame
