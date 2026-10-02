#pragma once
// The VR settings: what the VR menu (vr_menu.h) edits and the [vr] section of
// rrgame_settings.ini keeps (graphics_settings.h SettingsPath: saves\ next to rrgame.exe, files/saves on the Quest).
// A scripted run (a frame count, the desktop VR mock's shots) starts from the defaults and never reads the file;
// the command line's --vr-* flags set single values on top (ApplyVrFlag).
#include <string>

#include "vr_bars_settings.h"
#include "vr_melee_settings.h"
#include "vr_weapon_settings.h"

namespace rrgame {

struct VrSettings {
    int eyeScale = 100;          // % of the runtime's recommended eye image, 50..200 (applies at the next start)
    int refreshHz = 72;          // requested display rate (XR_FB_display_refresh_rate: 72 / 80 / 90 / 120 on Quest 3)
    int msaa = 2;                // 1 (off), 2, 4
    int drawDistance = 1;        // 0 original, 1 extended, 2 maximum (graphics_settings.h)
    bool maxDetail = true;       // every model at LOD 0, every cell group fine (graphics_settings.h)
    bool smoothTextures = true;  // bilinear + mipmaps
    bool hdMedia = true;         // "HD textures and media": the HD pack's pictures, fonts, HUD and films (docs/HD-MEDIA.md)
    int foveation = 2;           // 0 off, 1 low, 2 medium, 3 high (XR_FB_foveation)
    int vibration = 100;         // % of the rumble on the Touch haptics
    int horizonLock = 100;       // % of the camera's roll (and pitch, horizonMode kRollPitch) taken out (vr_rig.h)
    // what the lock levels - kRoll (the default): the bike's lean only, the pitch follows the bike on the
    // hills (the handlebars stay by the player's hands); kRollPitch: the pitch too (the full lock); kOff: nothing.
    enum HorizonMode { kHorizonOff = 0, kHorizonRoll = 1, kHorizonRollPitch = 2 };
    int horizonMode = kHorizonRoll;
    bool comfort = false;        // the comfort vignette: the view narrows while the bike turns
    int seatHeightCm = 0;        // the seated eye height adjustment, -40..40 cm
    int hudSize = 1;             // 0 small, 1 medium, 2 large (the bike-locked HUD panel)
    bool headView = true;        // the race starts on the rider's head (false: the original's chase camera)
    int worldScale = 100;        // % game metres per real metre (one world unit = one metre at 100)
    bool multiview = true;       // single-pass stereo (GL_OVR_multiview2) when the GPU has it (applies at the next start)
    // vr_comfort.h: the race drawn between its last two steps at the display's rate (smooth_motion), and
    // the view while the rider is off the bike - fixed and level behind him (fall_view=fixed) or the original's camera
    bool smoothMotion = true;
    bool fallFixed = true;
    // vr_bike_shake.h: the player's own bike's part slots in the head view - 0 off (the pitch spring at
    // rest), 1 low (interpolated, the pitch low-passed at half amplitude), 2 original (the game's slots). bike_shake
    int bikeShake = 1;
    // vr_horizon.h: the head view's bike pitch - 0 road (the grade only, low-passed), 1 low (the grade + a
    // quarter of the bike's own wheelie / stoppie pitch, low-passed), 2 original (the game's). view_pitch
    int viewPitch = 1;
    // the eye moved back along the bike (positive) or forward (negative) from the rider's head, -20..40 cm
    // (the eye on the bike: in the drawn bike's frame). seat_back_cm
    int seatBackCm = 0;
    BarsSettings bars;          // the steering: the stick or the handlebars in the hands (vr_bars_settings.h)
    MeleeSettings melee;         // the blows: physical contact, gesture or buttons (vr_melee_settings.h)
    WeaponsSettings weapons;     // the holsters, the weapon's grip, the swing's rules (vr_weapon_settings.h)

    bool Parse(const std::string& key, const std::string& value);
    std::string Serialize() const; // the [vr] section's lines
    std::string Describe() const;  // one log line

    // The product's defaults: what an interactive run starts from before the file (a key the file lacks keeps it). The
    // member initialisers above stay the scripted runs' baseline (the gates' races); these differ from them in the
    // eye resolution, the vibration, the seat height, the steering (the handlebars), the blow's minimum speed, the
    // holsters' weapons and the weapons' grips (docs\QUEST.md "Default settings").
    static VrSettings ProductDefaults();
    static const char* ProductDefaultsText(); // the same as [vr] key=value lines
};

VrSettings& VrPrefs();
// `rewrite` (optional): set when the section lacks a one-time migration's marker - the caller writes it back at once
// (SaveVrSettings), so the migration runs once.
bool LoadVrSettings(const std::string& path, VrSettings& s, bool* rewrite = nullptr);
bool SaveVrSettings(const std::string& path, const VrSettings& s);
// The VR flags of the command line (removed from argv by the caller): --vr-eye-scale P, --vr-refresh HZ, --vr-msaa N,
// --vr-horizon-lock P, --vr-horizon-mode off|roll|full, --vr-view head|chase, --vr-comfort 0|1, --vr-seat CM, --vr-hud 0..2, --vr-foveation 0..3,
// --vr-draw-distance original|extended|maximum, --vr-max-detail 0|1, --vr-multiview 0|1, --vr-smooth-motion 0|1,
// --vr-fall-view fixed|chase, --vr-bike-shake off|low|original, --vr-view-pitch road|low|original, --vr-seat-back CM (and vr_comfort.h's mock flags). False when argv[i] is not one; a bad value throws.
bool ApplyVrFlag(int argc, char** argv, int& i, VrSettings& s);

} // namespace rrgame
