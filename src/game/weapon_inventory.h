#pragma once
// The player's weapons as the game keeps them - the fields the VR holsters (tools\rrgame\vr_holsters.h)
// read and write, so ownership, theft and the career's carry-over stay the ORIGINAL's:
//
//   * OWNED: the runtime rider record's possession mask riderDef +0x2C bits 0..8 (rules.md 1.4 / 8.3; PickWeapon
//     RASHCDG 0x800B9340 tests `& 0x1FF`). WeaponSteal RASHCDG 0x800BFF04 moves the victim's CURRENT weapon (+0x2E)
//     into the thief's mask and clears it from the victim's - a rival that steals from the player removes it here,
//     exactly as the original. The career carries the four fields between races (RASHCDF 0x8007F628 before a race,
//     0x8007BC0C CareerResult after - shell_career.cpp CopyIdentity).
//   * IN HAND: +0x2E, 0..8 or 9 = fists; +0x2F the swings left on it; +0x30 eight per-weapon swing nibbles.
//
// The two writes (OURS; the fields and the values are the original's own):
//   * Draw(w): the weapon in hand becomes w exactly as PickWeapon selects one: +0x2E = w, +0x2F = its nibble of +0x30
//     (0 for weapon 8, which has no nibble - PickWeapon's `w < 8 ? nibble : 0`). Only an OWNED weapon.
//   * Holster(): the hand emptied as PickWeapon's unarmed branch does it: +0x2E = 9, +0x2F = 0 (the swings stay in the
//     weapon's nibble, where every writer of +0x2F keeps them - FightUpdate's swing, WeaponSteal).
// Neither is written while the rider holds a weapon OBJECT (rider +0x23B != 0xFF: WeaponObject RASHCDG 0x800958F0 seated
// one, a fight stance is under way - its clip and its release belong to the fight code); the caller retries a frame on.
#include <cstdint>
#include <string>

namespace rr::game {

struct WeaponRecord {
    bool valid = false;
    uint16_t owned = 0;   // riderDef +0x2C & 0x1FF
    int inHand = 9;       // riderDef +0x2E
    int swings = 0;       // riderDef +0x2F
    uint32_t nibbles = 0; // riderDef +0x30
    bool objectSeated = false; // rider +0x23B != 0xFF
};

// `ram`: the 2 MiB guest image; `rider` the pool-1 rider (its +0x23B), `rd` the runtime rider record (entity +0x43C).
WeaponRecord ReadWeaponRecord(const uint8_t* ram, uint32_t rider, uint32_t rd);
bool Owns(const WeaponRecord& r, int weapon);
// The swings weapon w would be drawn with (PickWeapon's rule).
int SwingsFor(const WeaponRecord& r, int weapon);

enum class WeaponWrite { kDone, kDeferred, kNotOwned, kBad };
WeaponWrite DrawWeapon(uint8_t* ram, uint32_t rider, uint32_t rd, int weapon);
WeaponWrite HolsterWeapon(uint8_t* ram, uint32_t rider, uint32_t rd);
const char* WeaponWriteName(WeaponWrite w);
std::string OwnedList(uint16_t owned); // "0,1,5" or "none"

} // namespace rr::game
