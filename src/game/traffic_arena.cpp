// The traffic's part of the race arena (traffic_arena.h), transcribed from our own listing of
// RASHCDI.BIN (SHA-1 9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06, loaded at 0x8005B5E8).
#include "game/traffic_arena.h"

#include "rrformats/level_bundle.h"
#include "game/loader_product.h" // the ported loader

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>

namespace rr::game {
namespace {

using rr::sim::GuestRam;

constexpr uint32_t kRegistry = 0x800CE1B0;     // 50 x 16
constexpr uint32_t kClassLists = 0x800CE560;   // 7 x 8: s16 count, s16 first slot, -> list
constexpr uint32_t kClassListArea = 0x800D4B88;
constexpr uint32_t kFamilies = 0x800D4C38;     // 7 x 16: count, used, base id, -> s16 slot by id
constexpr uint32_t kSlotById = 0x800CD6D8;     // 109 x s16
constexpr uint32_t kCapTable = 0x8006B49C;     // RASHCDI: 7 signed bytes
constexpr uint32_t kPoolTable = 0x800CE4D0;
constexpr uint32_t kPool3Ctrl = 0x800CF650;
constexpr uint32_t kPool3Slots = 0x800CF660;
constexpr uint32_t kTrafficBlock = 0x800D8710;
constexpr uint32_t kGameStatePtr = 0x8005B2F8;
constexpr uint32_t kRashcdiLoad = 0x8005B5E8;

constexpr uint32_t kTagRmd3 = 0x33444D52, kTagDod3 = 0x33444F44, kTagDpd3 = 0x33445044, kTagBbd3 = 0x33444242;
constexpr uint32_t kTagCtkp = 0x504B5443, kTagKnbp = 0x50424E4B, kTagTslp = 0x504C5354, kTagLect = 0x5443454C;

// OURS: the heap the loader's mallocs come from (a bump region; the console's is SLUS 0x800142B4).
struct Bump {
    uint32_t next = 0, limit = 0;
    bool failed = false;
    uint32_t Alloc(uint32_t n) {
        if (n == 0) return 0; // SLUS 0x8001447C: 0 for n == 0
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

struct Loader {
    GuestRam& g;
    Bump& heap;
    bool relocate = true; // false only for the check's negative control

    // RASHCDI 0x8005CB9C RmdHandler(id, chunk): the first free registry slot.
    int32_t Rmd(uint32_t id, uint32_t chunk) {
        for (int32_t k = 0; k < 50; ++k) {
            const uint32_t reg = kRegistry + 16u * static_cast<uint32_t>(k);
            if (g.U32(reg) != 0) continue;                                    // 0x8005CBC8
            g.W32(reg, id);                                                   // 0x8005CBD8
            const uint32_t gc = g.U8(chunk + 12);
            g.W8(reg + 4, static_cast<uint8_t>(gc));                          // 0x8005CBE4
            const uint32_t p = heap.Alloc(gc * 12u);                          // 0x8005CBF4
            g.W32(reg + 8, p);                                                // 0x8005CC0C
            if (p != 0) g.W32(p, 0);                                          // 0x8005CC08 memset(p, 0, 4)
            return k;
        }
        return -1;
    }
    // RASHCDI 0x8005CC4C DodHandler(id, chunk, gi).
    int32_t Dod(uint32_t id, uint32_t chunk, int32_t gi) {
        for (int32_t k = 0; k < 50; ++k) {
            const uint32_t reg = kRegistry + 16u * static_cast<uint32_t>(k);
            if (g.U32(reg) != id) continue;
            const uint32_t parts = heap.Alloc(4u * g.U16(chunk + 24));        // 0x8005CC94
            const uint32_t grp = g.U32(reg + 8) + 12u * static_cast<uint32_t>(gi);
            g.W32(grp + 4, parts);                                            // 0x8005CCA8
            g.W32(grp + 0, chunk);                                            // 0x8005CCB8
            if (relocate) {
                g.W32(chunk + 32, chunk + g.U32(chunk + 32));                 // 0x8005CCC8
                g.W32(chunk + 36, chunk + g.U32(chunk + 36));                 // 0x8005CCDC
                for (uint32_t off : {40u, 48u, 44u}) {                        // 0x8005CCE8 / 0x8005CD04 / 0x8005CD20
                    const uint32_t v = g.U32(chunk + off);
                    g.W32(chunk + off, v != 0 ? chunk + v : 0u);
                }
            }
            return k;
        }
        return -1;
    }
    // RASHCDI 0x8005CD60 DpdHandler(id, chunk, gi, si).
    int32_t Dpd(uint32_t id, uint32_t chunk, int32_t gi, int32_t si) {
        int32_t k = 0;
        for (; k < 50; ++k)
            if (g.U32(kRegistry + 16u * static_cast<uint32_t>(k)) == id) break;
        if (k == 50) return -1;
        if (relocate) {                                                       // 0x8005CE5C
            const uint32_t v = g.U32(chunk + 20);
            g.W32(chunk + 20, v != 0 ? chunk + v : 0u);
        }
        const uint32_t reg = kRegistry + 16u * static_cast<uint32_t>(k);
        const uint32_t grp = g.U32(reg + 8) + 12u * static_cast<uint32_t>(gi);
        g.W32(g.U32(grp + 4) + 4u * g.U8(chunk + 13), chunk);                 // 0x8005CDF8
        if (static_cast<uint32_t>(gi + 1) == g.U8(reg + 4) &&
            static_cast<uint32_t>(si + 1) == g.U16(g.U32(grp) + 24))
            g.W8(reg + 6, 1);                                                 // 0x8005CE34
        return 0;
    }
    // RASHCDI 0x8005CE78 BbdHandler(id, chunk, gi) with 0x8005C010 (the slot of `id`).
    int32_t Bbd(uint32_t id, uint32_t chunk, int32_t gi) {
        int32_t k = 0;
        for (; k < 50; ++k)
            if (g.U32(kRegistry + 16u * static_cast<uint32_t>(k)) == id) break;
        if (k == 50) return -1;
        const uint32_t reg = kRegistry + 16u * static_cast<uint32_t>(k);
        g.W32(g.U32(reg + 8) + 12u * static_cast<uint32_t>(gi) + 8u, chunk);  // 0x8005CEC8
        return 0;
    }
    // RASHCDI 0x8005C0C4 Walker(&out, buf, size, first).
    int32_t Walk(uint32_t& out, uint32_t buf, int32_t size, bool first) {
        uint32_t s8 = 0, s0 = buf;
        bool inModel = false;
        int32_t gi = -1, si = 0, slot = 0;
        while (static_cast<int32_t>(s0 - buf) < size) {
            const uint32_t tag = g.U32(s0), len = g.U32(s0 + 4), id = g.U32(s0 + 8);
            if (g.Faulted()) return -1;
            if (tag == kTagRmd3) {
                slot = Rmd(id, s0);
                if (slot == -1) return -1;
                inModel = true;
                s8 = id;
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
        g.W32(kRegistry + 16u * static_cast<uint32_t>(slot) + 12u, first ? buf : 0u);   // 0x8005C248 / 0x8005C258
        out = s8;
        return slot;
    }
    // RASHCDI 0x8005CA10's RMD3 loop over a GEO file already in guest memory at `file`.
    bool LoadGeo(uint32_t file, uint32_t bytes, std::vector<uint32_t>& models) {
        uint32_t s2 = file;
        bool first = true;
        while (static_cast<int32_t>(s2 - file) < static_cast<int32_t>(bytes)) {
            const uint32_t tag = g.U32(s2), len = g.U32(s2 + 4);
            if (tag == kTagRmd3) {
                uint32_t id = 0;
                const int32_t slot = Walk(id, s2, static_cast<int32_t>(len), first);
                if (slot < 0) return false;
                const uint32_t reg = kRegistry + 16u * static_cast<uint32_t>(slot);
                const uint32_t dod = g.U32(g.U32(reg + 8));
                // 0x8005C054: the texture key - the page table at *(0x8005B2E4) holds no page here
                // (the LECT arm uploads to VRAM and is not run), so no record matches: -1.
                g.W8(reg + 7, 0xFF);
                const uint32_t kind = (g.U16(dod + 14) & 0x78u) >> 3;
                ClassFirst(kind, static_cast<uint32_t>(slot));                  // 0x8005CB2C
                FamilyRegister(kind, id, static_cast<uint32_t>(slot));          // 0x8005CB50
                models.push_back(id);
                first = false;
            }
            if (len == 0) return false;
            s2 += len;
        }
        return !g.Faulted() && !heap.failed;
    }
    // RASHCDI 0x8005BD80(kind, slot).
    void ClassFirst(uint32_t kind, uint32_t slot) {
        const uint32_t c = kClassLists + 8u * kind;
        if (g.S16(c + 2) == -1) g.W16(c + 2, static_cast<uint16_t>(slot));
    }
    // SLUS 0x800303BC(kind, id, slot).
    void FamilyRegister(uint32_t kind, uint32_t id, uint32_t slot) {
        const uint32_t e = kFamilies + 16u * kind;
        const uint32_t p = g.U32(e + 12) + 2u * (id - g.U32(e + 8));
        if (g.S16(p) < 0) {
            g.W16(p, static_cast<uint16_t>(slot));
            g.W32(e + 4, g.U32(e + 4) + 1u);
        }
    }
};

// RASHCDI 0x8005D018 (the family tables) and 0x8005BE40 (the class lists, the registry).
void InitModelTables(GuestRam& g, const std::vector<uint8_t>& rashcdi) {
    for (uint32_t i = 0; i < 109; ++i) g.W16(kSlotById + 2u * i, 0xFFFF);
    struct F {
        uint32_t count, base, tableOff;
    };
    static const F kF[7] = {{0, 0, 0}, {21, 150, 0}, {21, 100, 42}, {20, 300, 84}, {45, 400, 124}, {1, 800, 214}, {1, 200, 216}};
    for (uint32_t k = 0; k < 7; ++k) {
        const uint32_t e = kFamilies + 16u * k;
        g.W32(e + 0, kF[k].count);
        g.W32(e + 4, 0);
        g.W32(e + 8, kF[k].base);
        g.W32(e + 12, k == 0 ? 0u : kSlotById + kF[k].tableOff);
    }
    uint32_t list = kClassListArea;
    for (uint32_t k = 0; k < 7; ++k) {
        const uint32_t c = kClassLists + 8u * k;
        g.W16(c + 2, 0xFFFF);
        g.W16(c + 0, 0);
        g.W32(c + 4, list);
        const size_t at = kCapTable - kRashcdiLoad + k;
        const int8_t cap = at < rashcdi.size() ? static_cast<int8_t>(rashcdi[at]) : 0;
        list += static_cast<uint32_t>(4 * static_cast<int32_t>(cap));
    }
    for (uint32_t i = 0; i < 800; i += 4) g.W32(kRegistry + i, 0);
}

// RASHCDI 0x8005C920's CTKP arm, 0x8005BDAC(chunk), on host bytes.
int WalkTexContainer(GuestRam& g, const uint8_t* p, size_t size, int& skipped) {
    int ctkp = 0;
    size_t at = 0;
    auto u32 = [&](size_t o) {
        uint32_t v;
        std::memcpy(&v, p + o, 4);
        return v;
    };
    while (at + 12 <= size) {
        const uint32_t tag = u32(at), len = u32(at + 4);
        if (tag == kTagCtkp) {
            ++ctkp;
            const uint32_t n = u32(at + 8);
            for (uint32_t i = 0; i < n && at + 12 + 8u * i + 8 <= size; ++i) {
                const uint8_t* e = p + at + 12 + 8u * i;
                const int16_t kind = static_cast<int16_t>(e[0] | (e[1] << 8));
                if (kind < 0 || kind > 6) continue; // the original indexes the table without a check
                const uint32_t c = kClassLists + 8u * static_cast<uint32_t>(kind);
                const uint32_t list = g.U32(c + 4);
                g.W16(list + 4u * i + 0, static_cast<uint16_t>(e[4] | (e[5] << 8)));
                g.W16(list + 4u * i + 2, static_cast<uint16_t>(e[6] | (e[7] << 8)));
                g.W16(c, static_cast<uint16_t>(g.U16(c) + 1u));
            }
        } else if (tag == kTagLect || tag == kTagKnbp || tag == kTagTslp) {
            ++skipped;
        }
        if (len == 0) break;
        at += len;
    }
    return ctkp;
}

std::vector<uint8_t> Read(const rr::DiscImage& disc, const std::string& path) {
    const auto f = disc.Find(path);
    return f ? disc.ReadFile(*f) : std::vector<uint8_t>{};
}

} // namespace

// A .GEO file through the loader's RMD3 walk (DATA\HAZARD<n>.GEO, world_pop_product.h).
bool LoadGeoIntoArena(GuestRam& g, const std::vector<uint8_t>& geo, uint32_t& from, uint32_t limit,
                      std::vector<uint32_t>& models, uint32_t* fileAt) {
    if (LoaderPorted()) { // GeoLoad RASHCDI 0x8005CA10 PORTED (loader_product.h)
        std::string error;
        return LoaderGeo(g, nullptr, nullptr, geo, from, limit, kLoaderSp, models, fileAt, error);
    }
    Bump heap;
    heap.next = (from + 7u) & ~7u;
    heap.limit = limit;
    const uint32_t file = heap.Alloc(static_cast<uint32_t>(geo.size()));
    if (file == 0) return false;
    g.WriteBlock(file, geo.data(), static_cast<uint32_t>(geo.size()));
    if (fileAt != nullptr) *fileAt = file;
    Loader ld{g, heap, true};
    if (!ld.LoadGeo(file, static_cast<uint32_t>(geo.size()), models)) return false;
    from = heap.next;
    return true;
}

TrafficArenaReport BuildTrafficArena(GuestRam& g, const rr::DiscImage& disc, int raceId, int players, uint32_t& from,
                                     uint32_t limit, bool relocate) {
    TrafficArenaReport r;
    const std::vector<uint8_t> rashcdi = Read(disc, "RASHCDI.BIN");
    if (rashcdi.empty()) {
        r.error = "RASHCDI.BIN is not on this disc";
        return r;
    }
    if (LoaderPorted()) { // RaceReset's ModelTablesInit RASHCDI 0x8005BE40 PORTED (once: the session
        // runs it before the bike bank, LoaderBikeBank)
        if (!LoaderTotals().tablesReady && !LoaderModelTables(g, nullptr, disc, kLoaderSp, r.error)) return r;
    } else {
        InitModelTables(g, rashcdi);
    }

    // The level bundle's texture sections: type 8 (0x8006270C, race-type flags & 0x18 == 0) and type 9
    // (0x8006275C, unless the flags are 8 outside race type 44), in the bundle's section order.
    const std::vector<uint8_t> bin = Read(disc, "DATA/GAMEBIN1.DAT");
    if (!bin.empty()) {
        try {
            const rr::LevelBundle bundle = rr::ParseLevelBundle(bin, rr::LevelBundleIndexForRace(raceId));
            const uint32_t gs = g.U32(kGameStatePtr);
            const uint32_t flags = g.U32(gs + 4) & 0x18u;
            const uint32_t raceType = g.U8(gs + 4);
            for (const rr::LevelBundleSection& s : bundle.sections) {
                const bool walk8 = s.type == 8 && flags == 0;
                const bool walk9 = s.type == 9 && !(flags == 8 && raceType != 44);
                if (!walk8 && !walk9) continue;
                const size_t len = static_cast<size_t>(s.tag) >= 4 ? static_cast<size_t>(s.tag) - 4 : 0;
                if (s.payload + len > bin.size()) continue;
                if (LoaderPorted() && relocate && ((from + 7u) & ~7u) + len <= limit) { // TexFile 0x8005C920 PORTED, the section transient
                    if (!LoaderTexSection(g, disc, bin.data() + s.payload, static_cast<uint32_t>(len), (from + 7u) & ~7u,
                                          r.ctkpChunks, r.texArmsSkipped, r.error))
                        return r;
                    continue;
                }
                r.ctkpChunks += WalkTexContainer(g, bin.data() + s.payload, len, r.texArmsSkipped);
            }
        } catch (const std::exception& e) {
            r.error = std::string("GAMEBIN1.DAT: ") + e.what();
            return r;
        }
    }

    Bump heap;
    heap.next = (from + 7u) & ~7u;
    heap.limit = limit;
    // The car file, RASHCDI 0x8005C630.
    if (LoaderPorted() && relocate) { // CarModels PORTED (the pedestrians' .GEO first, then the cars')
        uint32_t next = heap.next;
        if (!LoaderCarModels(g, nullptr, disc, raceId, next, limit, kLoaderSp, r.models, r.carFile, &r.fileAt, r.error))
            return r;
        heap.next = next;
        // RASHCDI 0x80068D54 (BuildRace's population reset) PORTED below; first the 16 part arrays pool 3 keeps
        // (BuildGrid's mallocs, OURS)
        // loader2: with the ported grid, PartSlotsInit RASHCDI 0x80069394 (BuildRace's population block, PORTED)
        // mallocs them and counts the capacity word; RRJB_LOADER2=off / the session's layout: OURS (placed here)
        const bool l2 = Loader2On() && Loader2Totals().gridPlanned;
        if (!l2)
            for (uint32_t s = 0; s < 16; ++s) g.W32(kPool3Slots + 512u * s + 4u, heap.Alloc(24));
        if (!LoaderPopReset(g, nullptr, disc, kLoaderSp, r.error)) return r;
        if (!l2) g.W32(kPool3Ctrl + 12, 16); // OURS: the pool allocator's capacity word, as every capture holds it
        g.W32(kPoolTable + 48, kPool3Slots);
        g.W32(kPoolTable + 52, 512);
        g.W32(kPoolTable + 56, kPool3Ctrl);
        g.W32(kPoolTable + 60, kPool3Ctrl + 8);
        r.classCars = g.S16(kClassLists + 24);
        if (heap.failed || g.Faulted()) {
            r.error = "the arena region ran out or a store faulted";
            return r;
        }
        from = heap.next;
        r.end = heap.next;
        r.ok = true;
        return r;
    }
    char name[40];
    std::snprintf(name, sizeof(name), "DATA/CAR%d%d%s.GEO", (raceId / 10) % 10, raceId % 10, players >= 2 ? "B" : "A");
    r.carFile = name;
    const std::vector<uint8_t> geo = Read(disc, name);
    if (geo.empty()) {
        r.error = std::string(name) + " is not on this disc";
        return r;
    }
    const uint32_t file = heap.Alloc(static_cast<uint32_t>(geo.size()));
    if (file == 0) {
        r.error = "no room in the arena for the car file";
        return r;
    }
    g.WriteBlock(file, geo.data(), static_cast<uint32_t>(geo.size()));
    r.fileAt = file;
    Loader ld{g, heap, relocate};
    if (!ld.LoadGeo(file, static_cast<uint32_t>(geo.size()), r.models)) {
        r.error = std::string("the loader transcription refused ") + name;
        return r;
    }

    // RASHCDI 0x80068D54, pool 3's part; the 16 part arrays first (BuildGrid's mallocs, OURS).
    for (uint32_t s = 0; s < 16; ++s) {
        const uint32_t slot = kPool3Slots + 512u * s;
        const uint32_t parts = heap.Alloc(24);
        for (uint32_t i = 0; i < 512; i += 4) g.W32(slot + i, 0);
        g.W32(slot + 4, parts);
    }
    g.W32(kPool3Ctrl + 0, 0);
    g.W32(kPool3Ctrl + 4, 0);
    g.W32(kPool3Ctrl + 8, 0xFFFFFFFFu);
    g.W32(kPool3Ctrl + 12, 16); // OURS: the pool allocator's capacity word, as every capture holds it
    const uint32_t np = g.U32(g.U32(kGameStatePtr) + 0x30);
    for (uint32_t p = 0; p < np && p < 2; ++p) {
        g.W32(kTrafficBlock + 4 + 4 * p, 16);
        g.W32(kTrafficBlock + 12 + 4 * p, 4);
    }
    g.W32(kTrafficBlock + 0x18, 4);
    g.W32(kTrafficBlock + 0x14, 16);
    g.W32(kTrafficBlock + 0x1C, 180);
    g.W32(kTrafficBlock + 0x20, 145);
    g.W16(kTrafficBlock + 0x24, 100);
    g.W16(kTrafficBlock + 0x26, 75);
    g.W16(kTrafficBlock + 0x28, 50);
    // Pool 3 in the pool table (OURS: the pool allocator's record, as every capture holds it).
    g.W32(kPoolTable + 48, kPool3Slots);
    g.W32(kPoolTable + 52, 512);
    g.W32(kPoolTable + 56, kPool3Ctrl);
    g.W32(kPoolTable + 60, kPool3Ctrl + 8);

    r.classCars = g.S16(kClassLists + 24);
    if (heap.failed || g.Faulted()) {
        r.error = "the arena region ran out or a store faulted";
        return r;
    }
    from = heap.next;
    r.end = heap.next;
    r.ok = true;
    return r;
}

TrafficArenaReport BuildTrafficArena(GuestRam& g, const rr::DiscImage& disc, int raceId, int players, uint32_t& from,
                                     uint32_t limit) {
    return BuildTrafficArena(g, disc, raceId, players, from, limit, true);
}

// ---------------------------------------------------------------------------- the check
int CheckTrafficArena(const rr::DiscImage& disc, const std::string& ramPath, bool mutate) {
    std::ifstream in(ramPath, std::ios::binary);
    std::vector<uint8_t> cap((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (cap.size() < 0x200000) {
        std::printf("trafficarenacheck: %s is not a 2 MiB RAM image\n", ramPath.c_str());
        return 2;
    }
    GuestRam c(cap.data(), 0x8005AC8C);
    const uint32_t gsp = c.U32(kGameStatePtr);
    if (gsp < 0x80000000u || gsp >= 0x80200000u) {
        std::printf("trafficarenacheck: %s REFUSED (no game_state)\n", ramPath.c_str());
        return 2;
    }
    const int raceId = static_cast<int>(c.U32(gsp + 0x40));
    const int players = static_cast<int>(c.U32(gsp + 0x30));
    if (raceId <= 0 || raceId > 99 || players < 1 || players > 2 || c.S16(kClassLists + 24) <= 0) {
        std::printf("trafficarenacheck: %s REFUSED (race %d, %d player(s), %d car classes: not a race)\n",
                    ramPath.c_str(), raceId, players, c.S16(kClassLists + 24));
        return 2;
    }
    if ((c.U32(gsp + 4) & 0x18u) == 8u && c.U8(gsp + 4) != 44u) {
        std::printf("trafficarenacheck: %s REFUSED (race-type flags 8: RASHCDI 0x8005C630 loads carSC.geo there, "
                    "which this builder does not transcribe)\n",
                    ramPath.c_str());
        return 2;
    }
    std::vector<uint8_t> mine(0x200000, 0);
    GuestRam g(mine.data(), 0x8005AC8C);
    g.W32(kGameStatePtr, gsp);
    for (uint32_t off : {0x04u, 0x30u, 0x40u}) g.W32(gsp + off, c.U32(gsp + off));
    uint32_t from = 0x80100000, limit = 0x80110000;
    const TrafficArenaReport r = BuildTrafficArena(g, disc, raceId, players, from, limit, !mutate);
    if (!r.ok) {
        std::printf("trafficarenacheck: build failed: %s\n", r.error.c_str());
        return 1;
    }
    size_t compared = 0, differ = 0;
    auto cmp = [&](uint32_t a, uint32_t b) {
        ++compared;
        if (a != b) ++differ;
    };
    // The class lists of kinds 3 and 4 (the bundle's CTKP), their counts and list pointers.
    for (uint32_t k = 3; k <= 4; ++k) {
        const uint32_t e = kClassLists + 8u * k;
        cmp(g.U16(e), c.U16(e));
        cmp(g.U32(e + 4), c.U32(e + 4));
        for (uint32_t i = 0; i < c.U16(e) && i < 16; ++i) cmp(g.U32(g.U32(e + 4) + 4 * i), c.U32(c.U32(e + 4) + 4 * i));
    }
    // Family 3: count, used, base, table pointer; then the slot-by-id table by model id.
    const uint32_t f3 = kFamilies + 48;
    for (uint32_t o = 0; o < 16; o += 4) cmp(g.U32(f3 + o), c.U32(f3 + o));
    std::map<uint32_t, uint32_t> slotMine, slotCap;
    for (uint32_t i = 0; i < 20; ++i) {
        const int16_t a = g.S16(g.U32(f3 + 12) + 2 * i), b = c.S16(c.U32(f3 + 12) + 2 * i);
        cmp(a < 0 ? 0u : 1u, b < 0 ? 0u : 1u);
        if (a >= 0) slotMine[300 + i] = static_cast<uint32_t>(a);
        if (b >= 0) slotCap[300 + i] = static_cast<uint32_t>(b);
    }
    // Each car model: the registry record, its LOD table, part arrays and the loaded chunks, every
    // pointer the loader made absolute compared relative to its own file's base.
    uint32_t capBase = 0;
    for (const auto& [id, s] : slotCap) {
        const uint32_t reg = kRegistry + 16 * s;
        if (c.U32(reg + 12) != 0) capBase = c.U32(reg + 12);
    }
    const uint32_t myBase = r.fileAt;
    for (const auto& [id, sc] : slotCap) {
        const auto it = slotMine.find(id);
        cmp(it != slotMine.end() ? 1u : 0u, 1u);
        if (it == slotMine.end()) continue;
        const uint32_t rm = kRegistry + 16 * it->second, rc = kRegistry + 16 * sc;
        cmp(g.U32(rm), c.U32(rc));
        cmp(g.U8(rm + 4), c.U8(rc + 4));
        cmp(g.U8(rm + 6), c.U8(rc + 6));
        cmp(g.U32(rm + 12) != 0 ? g.U32(rm + 12) - myBase : 0u, c.U32(rc + 12) != 0 ? c.U32(rc + 12) - capBase : 0u);
        for (uint32_t gi = 0; gi < c.U8(rc + 4); ++gi) {
            const uint32_t gm = g.U32(rm + 8) + 12 * gi, gc = c.U32(rc + 8) + 12 * gi;
            const uint32_t dm = g.U32(gm), dc = c.U32(gc);
            cmp(dm - myBase, dc - capBase);
            cmp(g.U32(gm + 8) != 0 ? g.U32(gm + 8) - myBase : 0u, c.U32(gc + 8) != 0 ? c.U32(gc + 8) - capBase : 0u);
            const uint32_t len = c.U32(dc + 4);
            for (uint32_t o = 0; o < len && o < 0x4000; o += 4) {
                const bool ptr = o >= 32 && o <= 48;
                const uint32_t a = g.U32(dm + o), b = c.U32(dc + o);
                if (ptr) cmp(a != 0 ? a - myBase : 0u, b != 0 ? b - capBase : 0u);
                else cmp(a, b);
            }
            for (uint32_t p = 0; p < c.U16(dc + 24); ++p) {
                const uint32_t pm = g.U32(g.U32(gm + 4) + 4 * p), pc = c.U32(c.U32(gc + 4) + 4 * p);
                cmp(pm != 0 ? pm - myBase : 0u, pc != 0 ? pc - capBase : 0u);
                if (pc != 0 && pm != 0) {
                    const uint32_t v = g.U32(pm + 20), w = c.U32(pc + 20);
                    cmp(v != 0 ? v - myBase : 0u, w != 0 ? w - capBase : 0u);
                }
            }
        }
    }
    // The traffic block's constants (+4 / +12 are rewritten by the ported Density every round).
    for (uint32_t o : {0x14u, 0x18u, 0x1Cu, 0x20u, 0x24u, 0x28u}) cmp(g.U32(kTrafficBlock + o), c.U32(kTrafficBlock + o));
    // Pool 3's pool-table record and capacity word.
    for (uint32_t o = 48; o < 64; o += 4) cmp(g.U32(kPoolTable + o), c.U32(kPoolTable + o));
    cmp(g.U32(kPool3Ctrl + 12), c.U32(kPool3Ctrl + 12));
    std::printf("trafficarenacheck: %s race %d, %s: %zu car model(s), %d CTKP chunk(s); %zu words compared, %zu "
                "differ%s\n",
                ramPath.c_str(), raceId, r.carFile.c_str(), r.models.size(), r.ctkpChunks, compared, differ,
                mutate ? " (MUTATED: no DOD3 / DPD3 relocation)" : "");
    return differ == 0 ? 0 : 1;
}

} // namespace rr::game
