// Weapons in the product (weapon_session.h).
#include "game/weapon_session.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "game/race_session.h"
#include "game/sim/traffic_bind.h"
#include "game/sim/weapon.h"
#include "game/sim/loader.h"      // RegistryFind RASHCDI 0x8005C010
#include "game/loader_product.h"  // the ported loader
#include "rrformats/level_bank.h"

namespace rr::game {
namespace {

using rr::sim::GuestRam;
namespace W = rr::sim::weapon;

constexpr uint32_t kRegistry = 0x800CE1B0;   // 50 x 16 (the model registry)
constexpr uint32_t kClassLists = 0x800CE560; // 7 x 8: s16 count, s16 first slot, -> list
constexpr uint32_t kFamilies = 0x800D4C38;   // 7 x 16: count, used, base id, -> s16 slot by id
constexpr uint32_t kGameStatePtr = 0x8005B2F8;
constexpr uint32_t kTagRmd3 = 0x33444D52, kTagDod3 = 0x33444F44, kTagDpd3 = 0x33445044, kTagBbd3 = 0x33444242;
constexpr int kWeaponPool = 5;

uint32_t HostU32(const std::vector<uint8_t>& f, size_t at) {
    uint32_t v = 0;
    std::memcpy(&v, f.data() + at, 4);
    return v;
}

// OURS: the loader's mallocs (SLUS 0x8001447C's block rule: size (n + 11) & ~7, the caller gets +4).
struct Bump {
    uint32_t next = 0, limit = 0;
    bool failed = false;
    uint32_t Alloc(uint32_t n) {
        if (n == 0) return 0;
        const uint32_t size = (n + 11u) & ~7u;
        if (next + size > limit) {
            failed = true;
            return 0;
        }
        const uint32_t user = next + 4u;
        next += size;
        return user;
    }
};

// RASHCDI 0x8005C0C4's walk over ONE RMD3 chunk, with its four handlers (the transcription
// traffic_arena.cpp's Loader carries, restated here for the one weapon model).
struct ChunkLoader {
    GuestRam& g;
    Bump& heap;
    int32_t Rmd(uint32_t id, uint32_t chunk) { // 0x8005CB9C: the first free registry slot
        for (int32_t k = 0; k < 50; ++k) {
            const uint32_t reg = kRegistry + 16u * static_cast<uint32_t>(k);
            if (g.U32(reg) != 0) continue;
            g.W32(reg, id);
            const uint32_t gc = g.U8(chunk + 12);
            g.W8(reg + 4, static_cast<uint8_t>(gc));
            const uint32_t p = heap.Alloc(gc * 12u);
            g.W32(reg + 8, p);
            if (p != 0) g.W32(p, 0);
            return k;
        }
        return -1;
    }
    int32_t Dod(uint32_t id, uint32_t chunk, int32_t gi) { // 0x8005CC4C
        for (int32_t k = 0; k < 50; ++k) {
            const uint32_t reg = kRegistry + 16u * static_cast<uint32_t>(k);
            if (g.U32(reg) != id) continue;
            const uint32_t parts = heap.Alloc(4u * g.U16(chunk + 24));
            const uint32_t grp = g.U32(reg + 8) + 12u * static_cast<uint32_t>(gi);
            g.W32(grp + 4, parts);
            g.W32(grp + 0, chunk);
            g.W32(chunk + 32, chunk + g.U32(chunk + 32));
            g.W32(chunk + 36, chunk + g.U32(chunk + 36));
            for (uint32_t off : {40u, 48u, 44u}) {
                const uint32_t v = g.U32(chunk + off);
                g.W32(chunk + off, v != 0 ? chunk + v : 0u);
            }
            return k;
        }
        return -1;
    }
    int32_t Dpd(uint32_t id, uint32_t chunk, int32_t gi, int32_t si) { // 0x8005CD60
        int32_t k = 0;
        for (; k < 50; ++k)
            if (g.U32(kRegistry + 16u * static_cast<uint32_t>(k)) == id) break;
        if (k == 50) return -1;
        const uint32_t v = g.U32(chunk + 20);
        g.W32(chunk + 20, v != 0 ? chunk + v : 0u);
        const uint32_t reg = kRegistry + 16u * static_cast<uint32_t>(k);
        const uint32_t grp = g.U32(reg + 8) + 12u * static_cast<uint32_t>(gi);
        g.W32(g.U32(grp + 4) + 4u * g.U8(chunk + 13), chunk);
        if (static_cast<uint32_t>(gi + 1) == g.U8(reg + 4) && static_cast<uint32_t>(si + 1) == g.U16(g.U32(grp) + 24))
            g.W8(reg + 6, 1);
        return 0;
    }
    int32_t Bbd(uint32_t id, uint32_t chunk, int32_t gi) { // 0x8005CE78
        int32_t k = 0;
        for (; k < 50; ++k)
            if (g.U32(kRegistry + 16u * static_cast<uint32_t>(k)) == id) break;
        if (k == 50) return -1;
        g.W32(g.U32(kRegistry + 16u * static_cast<uint32_t>(k) + 8) + 12u * static_cast<uint32_t>(gi) + 8u, chunk);
        return 0;
    }
    int32_t Walk(uint32_t buf, int32_t size) { // 0x8005C0C4, `first` = 0 (model 800 is not the file's first)
        uint32_t s0 = buf;
        bool inModel = false;
        int32_t gi = -1, si = 0, slot = -1;
        while (static_cast<int32_t>(s0 - buf) < size) {
            const uint32_t tag = g.U32(s0), len = g.U32(s0 + 4), id = g.U32(s0 + 8);
            if (g.Faulted() || (len == 0 && tag != kTagRmd3)) return -1;
            if (tag == kTagRmd3) {
                slot = Rmd(id, s0);
                if (slot == -1) return -1;
                inModel = true;
                s0 += 16;
                continue;
            }
            if (tag == kTagDod3) {
                ++gi;
                si = 0;
                if (inModel && Dod(id, s0, gi) == -1) return -1;
            } else if (tag == kTagDpd3) {
                if (Dpd(id, s0, gi, si) == -1) return -1;
                ++si;
            } else if (tag == kTagBbd3) {
                if (Bbd(id, s0, gi) == -1) return -1;
            }
            s0 += len;
        }
        if (slot >= 0) g.W32(kRegistry + 16u * static_cast<uint32_t>(slot) + 12u, 0u); // 0x8005C258
        return slot;
    }
};

// OURS: root counter 2 for the effect record's jitter - SLUS 0x8002705C reads it twice through
// GetRCnt; the console's counter runs at the system clock / 8, 14112 counts per 1/300 s tick of the
// race clock game_state+0x10, and that is what this answers (both reads of one call see one value).
rr::sim::SpineIo Io(GuestRam& g) {
    rr::sim::SpineIo io;
    io.rootCounter = (g.U32(g.U32(kGameStatePtr) + 0x10u) * 14112u) & 0xFFFFu;
    return io;
}

struct NoFreeFar final : W::WeaponCallees {
    RaceSession* s;
    explicit NoFreeFar(RaceSession* x) : s(x) {}
    bool FreeFarRider(uint32_t&) override {
        if (s != nullptr)
            s->NoteSeam("RASHCDG 0x8008CC94 FreeFarRider (ported with the recovery layer) is not served to the weapon "
                        "object: a nunchaku / chain drawn while all animation objects are in use is REFUSED");
        return false;
    }
};

} // namespace

WeaponCounts& WeaponTally() {
    static WeaponCounts counts;
    return counts;
}

WeaponArenaReport BuildWeaponArena(GuestRam& g, const rr::DiscImage& disc, int bank, uint32_t& from, uint32_t limit) {
    WeaponArenaReport rep;
    const uint32_t gsp = g.U32(0x8005B2F8u); // RASHCDI 0x8005C45C's bundle (rrformats/level_bank.h)
    rep.file = rr::LevelBankFile(rr::LevelBankIndex(bank, g.U8(gsp + 4u), g.S32(gsp + 0x48u)), ".GEO");
    const auto f = disc.Find(rep.file);
    if (!f) {
        rep.error = rep.file + " is not on the disc";
        return rep;
    }
    const std::vector<uint8_t> file = disc.ReadFile(*f);
    size_t at = 0, found = file.size();
    uint32_t len = 0;
    while (at + 16 <= file.size()) {
        const uint32_t tag = HostU32(file, at), l = HostU32(file, at + 4);
        if (l == 0) break;
        if (tag == kTagRmd3 && HostU32(file, at + 8) == W::kWeaponModel) {
            found = at;
            len = l;
            break;
        }
        at += l;
    }
    if (found == file.size() || found + len > file.size()) {
        rep.error = "no RMD3 of model 800 in " + rep.file;
        return rep;
    }
    const uint32_t fam = kFamilies + 16u * kWeaponPool;
    if (g.U32(fam + 8) != W::kWeaponModel || g.U32(fam + 12) == 0) {
        rep.error = "the model family tables (RASHCDI 0x8005D018, laid down with the traffic arena) are not there";
        return rep;
    }
    Bump heap;
    heap.next = (from + 7u) & ~7u;
    heap.limit = limit;
    rep.chunkAt = heap.next;
    rep.chunkBytes = len;
    // the PORTED LoadBikeBank RASHCDI 0x8005C45C registered the bank's .GEO whole (model 800 too)
    const int32_t loaded = LoaderPorted() ? rr::sim::RegistryFind(g, W::kWeaponModel) : -1;
    if (loaded >= 0) rep.chunkAt = g.U32(g.U32(kRegistry + 16u * static_cast<uint32_t>(loaded) + 8u));
    if (loaded < 0 && heap.next + len > limit) {
        rep.error = "no room for model 800";
        return rep;
    }
    if (loaded < 0) g.WriteBlock(heap.next, file.data() + found, len);
    if (loaded < 0) heap.next = (heap.next + len + 7u) & ~7u;
    ChunkLoader ld{g, heap};
    const int32_t slot = loaded >= 0 ? loaded : ld.Walk(rep.chunkAt, static_cast<int32_t>(len));
    if (slot < 0 || heap.failed || g.Faulted()) {
        rep.error = "the chunk walker refused model 800";
        return rep;
    }
    const uint32_t reg = kRegistry + 16u * static_cast<uint32_t>(slot);
    if (loaded < 0) g.W8(reg + 7, 0xFF); // 0x8005C054: no page table here (the LECT arm uploads to VRAM), so no key
    const uint32_t dod = g.U32(g.U32(reg + 8));
    rep.kind = static_cast<int>((g.U16(dod + 14) & 0x78u) >> 3);
    rep.groups = g.U8(reg + 4);
    rep.registrySlot = slot;
    if (loaded < 0) {   // 0x8005BD80 (the kind's first slot) and SLUS 0x800303BC (slot by id)
        const uint32_t c = kClassLists + 8u * static_cast<uint32_t>(rep.kind);
        if (g.S16(c + 2) == -1) g.W16(c + 2, static_cast<uint16_t>(slot));
        const uint32_t e = kFamilies + 16u * static_cast<uint32_t>(rep.kind);
        const uint32_t p = g.U32(e + 12) + 2u * (W::kWeaponModel - g.U32(e + 8));
        if (g.S16(p) < 0) {
            g.W16(p, static_cast<uint16_t>(slot));
            g.W32(e + 4, g.U32(e + 4) + 1u);
        }
    }
    // RASHCDI 0x80066600..0x80066628: ModelBind(0x800CF018 + 172 i, 5, 0, 1) for i < 8, the part array ours.
    // loader2: with the ported grid, SpawnBike's own loop (grid_build.cpp, PORTED) binds them - the original's.
    const uint32_t partBytes = 24u * g.U16(dod + 24);
    rep.atGrid = Loader2On() && Loader2Totals().gridPlanned && std::getenv("RRJB_LOADER_BIND") == nullptr;
    for (uint32_t i = 0; i < 8 && !rep.atGrid; ++i) {
        const uint32_t obj = W::kObjSlots + W::kObjBytes * i;
        const uint32_t parts = heap.Alloc(partBytes);
        if (parts == 0) {
            rep.error = "no room for the part arrays";
            return rep;
        }
        g.W32(obj + 4, parts);
        uint32_t v0 = 0;
        if (!rr::sim::ModelBind(g, obj, kWeaponPool, 0, 0, v0) || g.U32(obj + 96) != reg) {
            rep.error = "ModelBind (SLUS 0x8002FAD4, PORTED) refused weapon object " + std::to_string(i);
            return rep;
        }
        ++rep.bound;
    }
    rep.end = heap.next;
    from = heap.next;
    rep.ok = !g.Faulted() && rep.kind == kWeaponPool;
    if (!rep.ok && rep.error.empty()) rep.error = "model 800's DOD3 kind is not 5";
    return rep;
}

std::string BuildWeaponArenaLine(GuestRam& g, const rr::DiscImage& disc, int bank, uint32_t& from, uint32_t limit) {
    const WeaponArenaReport r = BuildWeaponArena(g, disc, bank, from, limit);
    char b[512];
    std::snprintf(b, sizeof(b),
                  "the weapon arena (weapon_session.h; RASHCDI 0x8005C0C4 and its handlers, 0x80066600 %s, "
                  "ModelBind SLUS 0x8002FAD4 PORTED): %s model 800, %d group(s), kind %d, registry slot %d (OURS: only "
                  "this model of the file, after the cars), %d of 8 weapon objects 0x800CF018 bound here, placed OURS at "
                  "0x%08X..0x%08X%s%s",
                  r.atGrid ? "PORTED in SpawnBike - the 8 weapon objects are bound at the grid" : "transcribed",
                  r.file.c_str(), r.groups, r.kind, r.registrySlot, r.bound, r.chunkAt, r.end,
                  r.ok ? "" : " - FAILED: ", r.error.c_str());
    return b;
}

// ---------------------------------------------------------------------------- the callees

bool ProductWeapon::Object(uint32_t r, uint32_t side) {
    NoFreeFar c(s_);
    uint32_t v0 = 0;
    ++WeaponTally().objects;
    const bool ok = W::WeaponObject(g_, c, r, side, Io(g_), v0) && !g_.Faulted();
    if (ok && v0 != 0) {
        ++WeaponTally().objectsDrawn;
        if (g_.U8(g_.U32(g_.U32(r + 0x254u) + 0x43Cu) + 47u) != 0) ++WeaponTally().effects; // its ObjectEffect ran
        if (g_.U32(r + 0x22Cu) != 0) ++WeaponTally().animObjects;
    }
    if (!ok) Refused("RASHCDG 0x800958F0 WeaponObject");
    return ok;
}

bool ProductWeapon::Release(uint32_t r, uint32_t slot, uint32_t z) {
    rr::sim::AnimMachine m(g_, pose_);
    m.SeatRelease(r, slot, z);
    ++WeaponTally().releases;
    if (m.Failed()) Refused("RASHCDG 0x80068D20 SeatRelease");
    return !m.Failed();
}

bool ProductWeapon::Stop(uint32_t slot) {
    rr::sim::CopLeave(g_, slot);
    ++WeaponTally().stops;
    if (g_.Faulted()) Refused("SLUS 0x8002847C CopLeave");
    return !g_.Faulted();
}

bool ProductWeapon::Effect(uint32_t e, int32_t a1, int32_t a2, int32_t a3, int32_t a4) {
    W::ObjectEffect(g_, e, static_cast<uint32_t>(a1), static_cast<uint32_t>(a2), static_cast<uint32_t>(a3),
                    static_cast<uint32_t>(a4), Io(g_));
    ++WeaponTally().effects;
    if (s_ != nullptr)
        s_->NoteSeam("SLUS 0x800273EC (PORTED) fills a weapon's effect record 0x800D39B0; nothing draws, ages or "
                     "frees the effect records (the effect pass and renderer are not ported); root counter 2 of "
                     "its jitter is ours (the race clock x 14112)");
    return !g_.Faulted();
}

bool ProductWeapon::Clip(uint32_t ev, uint32_t r, uint32_t a2, uint32_t a3, uint32_t& v0) {
    rr::sim::AnimMachine m(g_, pose_);
    const uint32_t before = g_.U32(r + 0x22Cu);
    ++WeaponTally().clips;
    const bool ok = W::OverlayClip(m, ev, r, a2, a3, v0) && !g_.Faulted();
    if (ok && before != 0) ++WeaponTally().clipStarts;
    if (!ok) Refused("RASHCDG 0x800C2F84 OverlayClip");
    return ok;
}

void ProductWeapon::Refused(const char* what) {
    ++WeaponTally().refused;
    if (s_ != nullptr)
        s_->NoteSeam(std::string(what) + " (PORTED) met an address this arena faults on: the fight step is refused "
                                         "and the arena restored");
}

// ---------------------------------------------------------------------------- the renderer's view

namespace {
uint32_t RamU32(const uint8_t* ram, uint32_t a) {
    uint32_t v = 0;
    std::memcpy(&v, ram + (a & 0x1FFFFFu), 4);
    return v;
}
} // namespace

bool ReadWeaponInHand(const uint8_t* ram, uint32_t rider, WeaponInHand& out) {
    if (ram == nullptr || rider < 0x80000000u || rider >= 0x80200000u) return false;
    const uint32_t obj = RamU32(ram, rider + 0x38u);
    if (obj < W::kObjSlots || obj >= W::kObjSlots + 8u * W::kObjBytes || (obj - W::kObjSlots) % W::kObjBytes != 0)
        return false;
    out.object = obj;
    out.hand = static_cast<int>(RamU32(ram, rider + 0x3Cu));
    out.group = static_cast<int8_t>(ram[(obj + 8u) & 0x1FFFFFu]);
    const uint32_t bike = RamU32(ram, rider + 0x254u);
    const uint32_t rd = (bike >= 0x80000000u && bike < 0x80200000u) ? RamU32(ram, bike + 0x43Cu) : 0u;
    out.weapon = (rd >= 0x80000000u && rd < 0x80200000u) ? ram[(rd + 46u) & 0x1FFFFFu] : 9;
    return true;
}

std::string WeaponLogLine(const uint8_t* ram, const uint32_t* riders, size_t n) {
    const WeaponCounts& t = WeaponTally();
    char b[256];
    std::snprintf(b, sizeof(b),
                  "weapon objects=%llu drawn=%llu anim=%llu clips=%llu started=%llu releases=%llu effects=%llu "
                  "refused=%llu |",
                  static_cast<unsigned long long>(t.objects), static_cast<unsigned long long>(t.objectsDrawn),
                  static_cast<unsigned long long>(t.animObjects), static_cast<unsigned long long>(t.clips),
                  static_cast<unsigned long long>(t.clipStarts), static_cast<unsigned long long>(t.releases),
                  static_cast<unsigned long long>(t.effects), static_cast<unsigned long long>(t.refused));
    std::string s = b;
    for (size_t i = 0; i < n; ++i) {
        const uint32_t bike = RamU32(ram, riders[i] + 0x254u);
        const uint32_t rd = (bike >= 0x80000000u && bike < 0x80200000u) ? RamU32(ram, bike + 0x43Cu) : 0u;
        const bool rdOk = rd >= 0x80000000u && rd < 0x80200000u;
        const int wpn = rdOk ? ram[(rd + 46u) & 0x1FFFFFu] : -1;
        const int swings = rdOk ? ram[(rd + 47u) & 0x1FFFFFu] : -1;
        WeaponInHand w;
        const bool held = ReadWeaponInHand(ram, riders[i], w);
        const bool bikeOk = bike >= 0x80000000u && bike < 0x80200000u;
        const int depth = bikeOk ? static_cast<int8_t>(ram[(bike + 0x3B2u) & 0x1FFFFFu]) : 0;
        const uint32_t top = bike + 0x3B4u + 8u * static_cast<uint32_t>(depth);
        const bool fighting = bikeOk && (RamU32(ram, top) & 0xFFFFu) == 16u;
        if (wpn == 9 && !held && !fighting) continue;
        if (fighting && rdOk) {
            // the fight record +0x239, the queue position +0x23A and its node, and the rider's animation key
            // (+0x21C's +0x10): what the emitter's weapon-glow test SLUS 0x80025218 reads
            const uint32_t rr = riders[i] & 0x1FFFFFu;
            const uint32_t anim = RamU32(ram, riders[i] + 0x21Cu);
            const int key = (anim >= 0x80000000u && anim < 0x80200000u) ? static_cast<int>(RamU32(ram, anim + 0x10u)) : -1;
            std::snprintf(b, sizeof(b), " r%zu[op16 cmd%u st%u own%03X rec%u q%u:%u key%d]", i, ram[(rd + 60u) & 0x1FFFFFu],
                          RamU32(ram, riders[i] + 0x220u) & 0xFFFFu, RamU32(ram, rd + 44u) & 0xFFFFu, ram[rr + 0x239u],
                          ram[rr + 0x23Au], ram[(rr + 0x23Eu + ram[rr + 0x23Au]) & 0x1FFFFFu], key);
            s += b;
        }
        std::snprintf(b, sizeof(b), " r%zu w%d s%d%s", i, wpn, swings,
                      held ? (" obj" + std::to_string((w.object - W::kObjSlots) / W::kObjBytes) + " hand" +
                              std::to_string(w.hand) + " lod" + std::to_string(w.group))
                                 .c_str()
                           : "");
        s += b;
    }
    return s;
}

std::string WeaponFrameLog(const RaceSession& s) {
    static WeaponCounts last;
    const WeaponCounts& t = WeaponTally();
    std::vector<uint32_t> riders;
    bool any = std::memcmp(&last, &t, sizeof(t)) != 0;
    for (const RaceBike& b : s.Bikes()) {
        riders.push_back(b.ownerAddress);
        WeaponInHand w;
        if (b.riderDef[0x2E] != 9 || ReadWeaponInHand(s.ArenaRam(), b.ownerAddress, w)) any = true;
    }
    last = t;
    const std::string line = WeaponLogLine(s.ArenaRam(), riders.data(), riders.size());
    if (!any && line.find("[op16") == std::string::npos) return {};
    return "      " + line + "\n";
}

std::string WeaponTotals() {
    const WeaponCounts& t = WeaponTally();
    char b[400];
    std::snprintf(b, sizeof(b),
                  "the PORTED weapon callees: WeaponObject 0x800958F0 %llu (a model object taken %llu, "
                  "with an animation object %llu), OverlayClip 0x800C2F84 %llu (on a weapon's clip %llu), SeatRelease "
                  "%llu, CopLeave %llu, ObjectEffect SLUS 0x800273EC %llu, refused %llu\n",
                  static_cast<unsigned long long>(t.objects), static_cast<unsigned long long>(t.objectsDrawn),
                  static_cast<unsigned long long>(t.animObjects), static_cast<unsigned long long>(t.clips),
                  static_cast<unsigned long long>(t.clipStarts), static_cast<unsigned long long>(t.releases),
                  static_cast<unsigned long long>(t.stops), static_cast<unsigned long long>(t.effects),
                  static_cast<unsigned long long>(t.refused));
    return b;
}

// DEVELOPMENT (rrgame --weapon W[:S]): the player's weapon in hand riderDef +0x2E := W, owned (+0x2C bit W),
// swings left +0x2F and its nibble of +0x30 := S - a data edit of the rider record, as a career carries weapons into a race
// (rules.md 8.3: RASHCDF 0x8007F628 copies +0x2C/+0x2E/+0x2F/+0x30 out of the career record).
namespace {
// The record's weapon fields for a DEVELOPMENT edit: owned (+0x2C bit w) and the weapon's own swing
// counter (its nibble of +0x30); `inHand` also makes it the weapon in hand (+0x2E) with +0x2F swings left.
void GiveWeapon(RaceBike& p, int weapon, int swings, bool inHand) {
    const uint8_t sw = static_cast<uint8_t>(swings < 0 ? 0 : (swings > 15 ? 15 : swings));
    if (inHand) {
        p.riderDef[0x2E] = static_cast<uint8_t>(weapon);
        p.riderDef[0x2F] = sw;
    }
    if (weapon >= 9) return;
    const uint16_t mask = static_cast<uint16_t>(p.riderDef[0x2C] | (p.riderDef[0x2D] << 8) | (1u << weapon));
    p.riderDef[0x2C] = static_cast<uint8_t>(mask & 0xFFu);
    p.riderDef[0x2D] = static_cast<uint8_t>(mask >> 8);
    if (weapon < 8) { // the nibble PickWeapon 0x800B9340 reloads +0x2F from
        const uint32_t sh = 4u * static_cast<uint32_t>(weapon);
        uint32_t w = static_cast<uint32_t>(p.riderDef[0x30]) | (static_cast<uint32_t>(p.riderDef[0x31]) << 8) |
                     (static_cast<uint32_t>(p.riderDef[0x32]) << 16) | (static_cast<uint32_t>(p.riderDef[0x33]) << 24);
        w = (w & ~(15u << sh)) | (static_cast<uint32_t>(sw) << sh);
        for (int k = 0; k < 4; ++k) p.riderDef[0x30 + k] = static_cast<uint8_t>(w >> (8 * k));
    }
}
} // namespace

// DEVELOPMENT (rrgame --opponent-weapons W[:S]): every opponent OWNS weapon W (+0x2C bit W, S swings in its
// nibble of +0x30) and has armed swings in its move list, as a career rider's record carries them into a
// race; the AI's own code (AiChooseCommand 0x800B8FB0's weapon arm, FightRestart's NextMove / PickWeapon
// 0x800B9340) decides whether and when it is drawn.
// W = 9 (the VR holsters' theft check): bare-fisted brawlers - no weapon, the loader's eight bare punches
// (32, the command WeaponSteal 0x800BFF04 steals with) kept, only the grudge and the mood below.
void RaceSession::DevOpponentWeapons(int weapon, int swings) {
    if (weapon < 0 || weapon > 9) return;
    for (size_t i = 1; i < bikes_.size(); ++i) {
        if (weapon < 9) GiveWeapon(bikes_[i], weapon, swings, false);
        // and an armed swing in its move list (+0x34..+0x3B, NextMove 0x800B92C0's choices): the loader's
        // default is eight bare punches (32); rr-race's career records carry armed moves (147, 148) there
        if (weapon < 9)
            for (int k = 0; k < 8; k += 2) bikes_[i].riderDef[0x34 + k] = 142;
        // and a grudge against the player at its full value with the mood nibble +0x02 raised, so that
        // the planner's AiChooseCommand takes its weapon arm (idx 6: aggression over both class thresholds,
        // the mood gate) - the grudge slot k whose handle map entry 0x800D38C8[k] is the player's handle
        const uint16_t ph = static_cast<uint16_t>(bikes_[0].entity[0xAC] | (bikes_[0].entity[0xAD] << 8));
        for (uint32_t k = 0; k < 20; ++k)
            if (*At(0x800D38C8u + k) == ph) bikes_[i].riderDef[0x10 + k] = 127;
        bikes_[i].riderDef[0x02] = static_cast<uint8_t>((bikes_[i].riderDef[0x02] & 0xF0u) | 0x0Fu);
    }
    if (weapon >= 9) {
        NoteSeam("DEVELOPMENT: every opponent bare-fisted (the loader's moves) with a grudge of 127 against the player and "
                 "mood 15 (rrgame --opponent-weapons 9)");
        return;
    }
    NoteSeam("DEVELOPMENT: every opponent owns weapon " + std::to_string(weapon) + " (riderDef+0x2C, " +
             std::to_string(swings) + " swing(s) in +0x30) and moves 0/2/4/6 of +0x34 are the armed swing 142 "
             "; a grudge of 127 against the player and mood 15 (rrgame --opponent-weapons)");
}

void RaceSession::DevPlayerWeapon(int weapon, int swings) {
    if (bikes_.empty() || weapon < 0 || weapon > 9) return;
    RaceBike& p = bikes_[0];
    GiveWeapon(p, weapon, swings, true);
    NoteSeam("DEVELOPMENT: the player's weapon riderDef+0x2E was set to " + std::to_string(weapon) + " with " +
             std::to_string(p.riderDef[0x2F]) + " swing(s) left (rrgame --weapon)");
}

} // namespace rr::game
