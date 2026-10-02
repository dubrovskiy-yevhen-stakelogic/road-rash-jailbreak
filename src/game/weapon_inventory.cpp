// The player's weapons as the game keeps them (weapon_inventory.h).
#include "game/weapon_inventory.h"

#include <cstring>

namespace rr::game {

namespace {

bool InRam(uint32_t a) { return a >= 0x80000000u && a < 0x80200000u; }
uint8_t* At(uint8_t* ram, uint32_t a) { return ram + (a & 0x1FFFFFu); }
const uint8_t* At(const uint8_t* ram, uint32_t a) { return ram + (a & 0x1FFFFFu); }

} // namespace

WeaponRecord ReadWeaponRecord(const uint8_t* ram, uint32_t rider, uint32_t rd) {
    WeaponRecord r;
    if (ram == nullptr || !InRam(rd) || !InRam(rider)) return r;
    uint16_t mask = 0;
    std::memcpy(&mask, At(ram, rd + 0x2Cu), 2);
    r.owned = static_cast<uint16_t>(mask & 0x1FFu);
    r.inHand = *At(ram, rd + 0x2Eu);
    r.swings = *At(ram, rd + 0x2Fu);
    std::memcpy(&r.nibbles, At(ram, rd + 0x30u), 4);
    r.objectSeated = *At(ram, rider + 0x23Bu) != 0xFFu;
    r.valid = true;
    return r;
}

bool Owns(const WeaponRecord& r, int weapon) { return weapon >= 0 && weapon <= 8 && ((r.owned >> weapon) & 1u) != 0; }

int SwingsFor(const WeaponRecord& r, int weapon) { // PickWeapon 0x800B9340's last store
    return weapon >= 0 && weapon < 8 ? static_cast<int>((r.nibbles >> (4 * weapon)) & 0xFu) : 0;
}

WeaponWrite DrawWeapon(uint8_t* ram, uint32_t rider, uint32_t rd, int weapon) {
    const WeaponRecord r = ReadWeaponRecord(ram, rider, rd);
    if (!r.valid || weapon < 0 || weapon > 8) return WeaponWrite::kBad;
    if (!Owns(r, weapon)) return WeaponWrite::kNotOwned;
    if (r.objectSeated) return WeaponWrite::kDeferred;
    *At(ram, rd + 0x2Eu) = static_cast<uint8_t>(weapon);
    *At(ram, rd + 0x2Fu) = static_cast<uint8_t>(SwingsFor(r, weapon));
    return WeaponWrite::kDone;
}

WeaponWrite HolsterWeapon(uint8_t* ram, uint32_t rider, uint32_t rd) {
    const WeaponRecord r = ReadWeaponRecord(ram, rider, rd);
    if (!r.valid) return WeaponWrite::kBad;
    if (r.objectSeated) return WeaponWrite::kDeferred;
    *At(ram, rd + 0x2Eu) = 9; // PickWeapon's unarmed branch (0x800B9340: sb 9, 46(a0); sb zero, 47(a0))
    *At(ram, rd + 0x2Fu) = 0;
    return WeaponWrite::kDone;
}

const char* WeaponWriteName(WeaponWrite w) {
    switch (w) {
    case WeaponWrite::kDone: return "done";
    case WeaponWrite::kDeferred: return "deferred (a weapon object in the hand)";
    case WeaponWrite::kNotOwned: return "not owned";
    default: return "no record";
    }
}

std::string OwnedList(uint16_t owned) {
    std::string s;
    for (int w = 0; w <= 8; ++w)
        if ((owned >> w) & 1u) s += (s.empty() ? "" : ",") + std::to_string(w);
    return s.empty() ? "none" : s;
}

} // namespace rr::game
