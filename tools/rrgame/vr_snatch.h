#pragma once
// VR weapon snatching - OURS, an addition to the VR physical combat (vr_melee.h): while a
// rival swings his weapon at the player, the player's FREE hand closing its grip on that weapon takes it.
//
//   * The rival's weapon: the weapon OBJECT the original seats in his hand for a fight stance (rider +0x38, a model
//     object of 0x800CF018; its LOD +0x08 = the weapon, the seat kind +0x3C = the hand part 7 / 10 - weapon_draw.h),
//     posed exactly as the renderer draws it - the hand part's world frame from the ported pose (vr_melee.cpp
//     ComputeTarget) and the object's own part slots for the jointed ones (nunchaku, chain) - as a capsule from the
//     attach point to the posed weapon's farthest vertex, radius 5 cm.
//   * The grab: a free hand (tracked, not on the handlebars, not the hand holding the player's own weapon) whose grip
//     button went from open to closed (>= 50 %) at most 0.25 s ago, the palm (a sphere of "Fist size" round the grip
//     pose) touching that capsule - swept between the last frame and this one, both moving, so a fast swing cannot pass
//     through the hand between two frames. Only while the rival SWINGS: the original moves a weapon from one rider to
//     another only through WeaponSteal RASHCDG 0x800BFF04, and only mid-swing (fight_physical.h PhysicalSnatchRun), so
//     a weapon the rival just holds cannot be grabbed (the original's rule; and a hand that brushes a held weapon while
//     riding alongside must not disarm anybody).
//   * The hold: while the grip stays closed and the hand within 35 cm of the weapon, up to 0.6 s, the grab is sent to
//     the game once a frame (PadState::blow with `snatch`, race_session.h) and applied through the ORIGINAL's theft
//     (fight_session.cpp FightPhysicalBlow -> PhysicalSnatchRun -> WeaponSteal): too early in his swing it is held and
//     asked again; in WeaponSteal's window the weapon changes hands - ownership (+0x2C, so the holster inventory,
//     weapon_inventory.h, has it), the weapon in hand, the swings, the HUD icon, the steal sound, his reaction stance
//     are the original's; declined, the player's punch goes on as a pad punch's would (FightUpdate offers the steal
//     again). The game's answer comes back through rr::game::LastSnatch() (fight_session.h).
//   * Taken: the weapon goes into the hand that grabbed it (it becomes the weapon hand, VrPrefs().melee.weaponHand), a
//     strong haptic pulse on that hand, the log line `vr snatch: ... TAKEN`.
//   * The settings: [vr] melee_snatch=1 (on by default in the Physical mode; the menu's Combat options), and the mock's
//     script command `snatch H [miss] [N]` (vr_melee.cpp): the scripted hand tracks the nearest rival's weapon, open,
//     and closes on it when he swings it (`miss`: 45 cm above it - the control); N attempts.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "rrformats/pose.h"
#include "rrformats/rmd3.h"
#include "rrformats/skeleton.h"
#include "rrvfs/disc_image.h"

namespace rr::game {
struct PhysicalBlowRequest;
}

namespace rrgame {

class VrSnatch {
public:
    // Model 800's groups (the weapons) and the attachment programs, for the rivals' posed weapons.
    bool Load(const std::vector<rr::Model>& models, const rr::SkeletonTable& skeleton);

    // ---- the rivals (vr_melee.cpp UpdateTargets / ComputeTarget, once per game frame)
    void BeginRivals();
    // The weapon object rider `rider` holds, posed: `toWorld(local, part)` maps part-local model units of the rider's
    // posed part to the world (ComputeTarget's). False when he holds none.
    bool HeldWeapon(const uint8_t* ram, uint32_t rider, const std::function<void(const float*, size_t, float*)>& toWorld,
                    float a[3], float b[3], int& weapon) const;
    void Rival(uint32_t bike, size_t index, bool hittable, bool held, const float a[3], const float b[3], int weapon,
               const float origin[3], const float right[3], const float up[3], const float fwd[3], bool swinging);
    // The rival on bike `bike` mid-swing with a weapon: his stance's category is the strike's (3, the stance table
    // SLUS 0x800541D4 +2, as FightUpdate / WeaponSteal test it) and his command the armed one (bit 0x80). Only the
    // mock's scripted hand looks at it (when to close); the grab itself is decided by the game (PhysicalSnatchRun).
    static bool Swinging(const uint8_t* ram, uint32_t bike);
    void EndRivals(int playerWeapon);

    // ---- the hands (vr_melee.cpp UpdateHands, once per VR frame, before the blows' sweep)
    struct HandIn {
        bool valid = false, free = false; // tracked; not on the bars, not holding the player's weapon
        float grip = 0.0f;
        float pos[3] = {};                // the grip pose (the palm's centre), world
        float radius = 0.0f;              // the palm sphere, world units
        float reach = 0.0f;               // metres from the seat's eye point (the log)
    };
    // `clock`: seconds of hand time; `upm`: world units per metre. Returns the requests to send this frame.
    void Update(const HandIn hands[2], double clock, float dt, float upm, long frame, bool active,
                std::vector<rr::game::PhysicalBlowRequest>& out);
    bool Holding(int hand) const;      // a hand holding a rival's weapon (no fist blows from it meanwhile)
    int TakenHand();                   // the hand that just took a weapon (once), -1 none
    bool CaughtNow() const { return caughtFrame_ == frame_; } // a hand closed on a weapon this frame
    int CaughtHand() const { return caughtHand_; }
    int TakenWeapon() const { return takenWeapon_; }

    // ---- the mock's script (vr_melee.cpp ScriptHands): the nearest rival weapon to `eye` within `range`
    struct RivalWeapon {
        uint32_t bike = 0;
        size_t index = 0;
        bool hittable = false, have = false, havePrev = false;
        int weapon = 9;
        float a[3] = {}, b[3] = {}, prevA[3] = {}, prevB[3] = {};
        float origin[3] = {}, right[3] = {}, up[3] = {}, fwd[3] = {};
        float prevOrigin[3] = {};
        float speed = 0.0f;              // the capsule's tip speed relative to its rider's bike, world units / s
        bool swinging = false;           // Swinging()
    };
    const RivalWeapon* Nearest(const float eye[3], float range) const;
    const std::vector<RivalWeapon>& Rivals() const { return rivals_; }

    // The debug lines (vr_melee.cpp Draw, "Show colliders"): the rivals' weapons orange (red while held by a hand).
    void Lines(const float eye[3], float range, const std::function<void(const float*, const float*, float, const float*)>& capsule) const;
    std::string Totals() const;

private:
    struct Group {
        rr::ModelGroup group;
        rr::Assembly assembly;
    };
    std::vector<Group> groups_;
    const rr::SkeletonTable* skeleton_ = nullptr;
    std::vector<RivalWeapon> rivals_, next_;
    int playerWeapon_ = 9;
    struct Hold {
        bool active = false;
        uint32_t bike = 0;
        size_t index = 0;
        int weapon = 9;
        double since = 0.0;
        bool declined = false;       // the game started the punch, WeaponSteal declined: watch, do not resend
        uint64_t sentSerial = 0;     // rr::game::LastSnatch().serial when the last request was sent
        int sent = 0;
    } hold_[2];
    struct HandState {
        float prevGrip = 0.0f;
        double closedAt = -10.0;
        bool havePrev = false;
        float prevPos[3] = {};
    } hand_[2];
    int takenHand_ = -1, takenWeapon_ = 9;
    long caughtFrame_ = -1;
    int caughtHand_ = -1;
    long frame_ = 0;
    float upm_ = 1.0f;
    double lastDt_ = 1.0 / 72.0;
    struct Totals {
        size_t closes = 0, catches = 0, sent = 0, taken = 0, waits = 0, declined = 0, skipped = 0, dropped = 0;
        size_t notSwinging = 0, missed = 0;
        std::string list;
    } totals_;
    void Finish(int h, const char* why);
};

} // namespace rrgame
