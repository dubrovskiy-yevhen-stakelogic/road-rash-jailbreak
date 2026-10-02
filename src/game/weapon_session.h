#pragma once
// Weapons in the product: the eight weapon model objects the race
// loader binds, the fight code's weapon callees served by the PORTED functions of
// src\game\sim\weapon.h (and the ported SeatRelease / CopLeave they release through), and what the
// renderer reads back to draw a weapon in a rider's hand.
#include <cstddef>
#include <cstdint>
#include <string>

#include "game/sim/anim.h"
#include "game/sim/road_query.h"
#include "rrvfs/disc_image.h"

namespace rr::game {

class RaceSession;

// ---------------------------------------------------------------------------- the arena
// The race loader's weapon part, transcribed from our own listing of RASHCDI.BIN (SHA-1
// 9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06): the RMD3 of model 800 out of DATA\BBLEVEL<bank+1>.GEO
// through the loader's chunk walker (0x8005C0C4 with the RMD3 / DOD3 / DPD3 / BBD3 handlers 0x8005CB9C /
// 0x8005CC4C / 0x8005CD60 / 0x8005CE78, the kind's first slot 0x8005BD80, SLUS 0x800303BC slot by id -
// the same transcription traffic_arena.cpp runs on the car file), then the loop at 0x80066600: the eight
// 172-byte objects 0x800CF018 bound by the PORTED ModelBind(obj, 5, 0, ...) (SLUS 0x8002FAD4).
// OURS, named: only model 800 of the file is loaded (after the car models, so it takes the next free
// registry slot, not the captures' 5), the file's bytes and the loader's mallocs sit in a bump region
// [from, limit), and each object's part array (the group-0 part count x 24 = 96 bytes, RegistryBind's
// malloc) is laid down by us, with ModelBind's `alloc` 0. Needs the family tables BuildTrafficArena
// lays down (RASHCDI 0x8005D018: family 5 = model 800).
struct WeaponArenaReport {
    bool ok = false;
    std::string error;
    std::string file;
    int registrySlot = -1;
    uint32_t chunkAt = 0, chunkBytes = 0, end = 0;
    int groups = 0, kind = -1;
    int bound = 0; // model objects ModelBind bound (8)
    bool atGrid = false; // loader2: SpawnBike's own loop binds them at the grid
};
WeaponArenaReport BuildWeaponArena(rr::sim::GuestRam& g, const rr::DiscImage& disc, int bank, uint32_t& from,
                                   uint32_t limit);
// The same, as the session's one-line hook: the seam-list line that names what was built.
std::string BuildWeaponArenaLine(rr::sim::GuestRam& g, const rr::DiscImage& disc, int bank, uint32_t& from,
                                 uint32_t limit);

// ---------------------------------------------------------------------------- the fight code's callees
// Per run: what the weapon callees did (rrgame's frame log and run tail).
struct WeaponCounts {
    uint64_t objects = 0;        // WeaponObject 0x800958F0 ran
    uint64_t objectsDrawn = 0;   // ... and took a model object (v0 1)
    uint64_t animObjects = 0;    // ... with an animation object (weapon 0 / 4)
    uint64_t clips = 0;          // OverlayClip 0x800C2F84 ran
    uint64_t clipStarts = 0;     // ... and started / restarted the weapon's clip
    uint64_t releases = 0;       // SeatRelease 0x80068D20 (ReleaseRiderObject's)
    uint64_t stops = 0;          // CopLeave SLUS 0x8002847C on a model object (its effect records)
    uint64_t effects = 0;        // ObjectEffect SLUS 0x800273EC
    uint64_t refused = 0;        // a callee refused (FreeFarRider is not served; or a fault)
};
WeaponCounts& WeaponTally();

// The five weapon callees of rr::sim::fight::Callees on the session's arena. `pose` is the session's
// rider seam (the animation machine's pose side), `session` names the seams.
class ProductWeapon {
public:
    ProductWeapon(rr::sim::GuestRam& g, rr::sim::AnimPoseSeam& pose, RaceSession* session)
        : g_(g), pose_(pose), s_(session) {}
    bool Object(uint32_t r, uint32_t side);                                           // RASHCDG 0x800958F0
    bool Release(uint32_t r, uint32_t slot, uint32_t z);                              // RASHCDG 0x80068D20
    bool Stop(uint32_t slot);                                                         // SLUS 0x8002847C
    bool Effect(uint32_t e, int32_t a1, int32_t a2, int32_t a3, int32_t a4);          // SLUS 0x800273EC
    bool Clip(uint32_t ev, uint32_t r, uint32_t a2, uint32_t a3, uint32_t& v0);       // RASHCDG 0x800C2F84

private:
    void Refused(const char* what);
    rr::sim::GuestRam& g_;
    rr::sim::AnimPoseSeam& pose_;
    RaceSession* s_;
};

// ---------------------------------------------------------------------------- the renderer's view
// Rider `r`'s weapon in hand: seat 0 of the rider (+0x38) holds a model object of 0x800CF018 (the
// seat the ported Attach wrote), `hand` its kind +0x3C - the part whose frame the attachment program
// hands the object (RASHCDG 0x80067064's control word: 7 or 10, the ends of the two arms) - and
// `group` its LOD +0x08 (the weapon: group k of model 800). False when the rider holds none.
struct WeaponInHand {
    uint32_t object = 0;
    int hand = 0;
    int group = 0;
    int weapon = 9;          // riderDef +0x2E
};
bool ReadWeaponInHand(const uint8_t* ram, uint32_t rider, WeaponInHand& out);

// One line for the frame log: every rider with a weapon object, and the tallies.
std::string WeaponLogLine(const uint8_t* ram, const uint32_t* riders, size_t n);
// rrgame's frame log: that line when a rider holds a weapon or a callee ran this frame, else "".
std::string WeaponFrameLog(const RaceSession& s);
// The run tail.
std::string WeaponTotals();

} // namespace rr::game
