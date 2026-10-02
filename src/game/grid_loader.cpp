#include "game/grid_loader.h"
#include "game/world_pop_product.h" // SpawnBikeClass
#include "game/rider_model.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "game/sim/rider_record.h"

namespace rr::game {

bool ReadStartBlock(const DiscImage& disc, int set, int32_t raceId, std::vector<StartEntry>& out, std::string& why) {
    out.clear();
    const std::string name = std::string("DATA/STARTDF") + (set == 2 ? "B" : "A") + ".BIN";
    std::vector<uint8_t> f;
    if (const auto e = disc.Find(name)) f = disc.ReadFile(*e);
    const size_t at = 292u * static_cast<size_t>(raceId - 1);
    if (raceId < 1 || f.size() < at + 292u) {
        why = name + " is missing or holds no block " + std::to_string(raceId - 1);
        return false;
    }
    auto s32 = [&](size_t o) {
        int32_t v;
        std::memcpy(&v, f.data() + at + o, 4);
        return v;
    };
    const int32_t n = s32(0);
    if (n < 0 || 4 + 12 * n > 292) {
        why = name + " block " + std::to_string(raceId - 1) + " has an entry count out of range";
        return false;
    }
    for (int32_t i = 0; i < n; ++i) out.push_back({s32(4u + 12u * i), s32(8u + 12u * i), s32(12u + 12u * i)});
    return true;
}

namespace {
constexpr uint32_t kGsPtr = 0x8005B2F8, kPlayerRecords = 0x800D81D8, kP1Bike = 0x8005B38C, kBikeCount = 0x8005B1F8;

// RASHCDI 0x80065648 PlayerColumn: t = (s16)gs+0x3A; ((0x148 >> 4t) & 15) - min(gs+0x44 / ((0x8532 >> 4t) & 15),
// (0x123 >> 4t) & 15) - the divide signed (a zero divisor answers as the R3000's `div` does).
int32_t PlayerColumn(rr::sim::GuestRam& g) {
    const uint32_t gs = g.U32(kGsPtr);
    const uint32_t t = static_cast<uint32_t>(static_cast<int32_t>(g.S16(gs + 58u)) << 2) & 31u;
    const int32_t d = static_cast<int32_t>((0x8532u >> t) & 15u);
    const int32_t w = g.S32(gs + 68u);
    const int32_t q = d == 0 ? (w >= 0 ? -1 : 1) : w / d;
    const int32_t cap = static_cast<int32_t>((291u >> t) & 15u);
    const int32_t a1 = q < cap ? q : cap;
    return static_cast<int32_t>((328u >> t) & 15u) - a1;
}
} // namespace

SpawnIndex SpawnAiIndex(rr::sim::GuestRam& g, int32_t slot, int player, uint32_t& mask) {
    SpawnIndex r;
    const uint32_t gs = g.U32(kGsPtr);
    if (player >= 0) {                                              // flags & 9 (0x80065B60, 0x80065FD4)
        const uint32_t p = static_cast<uint32_t>(player) & 1u;
        const uint32_t cls = g.U32(gs + 72u + 4u * p) / 9u;         // the 0x38E38E39 idiom, unsigned (0x80065FF4)
        r.ai = p;
        if (cls == 2u) {
            r.record = 26;
        } else {
            const int32_t b10 = g.S8(kPlayerRecords + 36u * p + 10u);
            r.record = b10 > 0 ? static_cast<uint32_t>(2 * (b10 - 1)) + cls + 22u : cls;
        }
        int32_t col = PlayerColumn(g);
        const uint32_t rd1 = (cls | (static_cast<uint32_t>(col) << 4)) & 0xFFu;
        r.riderDef1 = static_cast<int32_t>(rd1);
        if (col < 2) col = 8;
        if (p == 1u) {                                              // a second player of player 1's class
            const uint32_t p1 = g.U32(kP1Bike);
            if ((rd1 & 0xFu) == (g.U8(g.U32(p1 + 1084u) + 1u) & 0xFu)) col = (col == 5 || col == 2) ? col + 1 : col - 1;
        }
        mask |= 1u << ((static_cast<uint32_t>(col) + 10u * rd1) & 31u); // 0x800660A8
        return r;
    }
    if (!(static_cast<uint32_t>(slot) < 17u)) {                     // police (0x80065BE8)
        r.ai = r.record = static_cast<uint32_t>(slot) + 1u;
        return r;
    }
    uint32_t s = static_cast<uint32_t>(slot), col = 0, cls = 0;
    for (;;) {                                                      // 0x80065C0C
        const uint32_t v = s + 1u;
        col = v >> 1;
        cls = v & 1u;
        while (mask & (1u << ((col + 10u * cls) & 31u))) ++col;
        if (static_cast<int32_t>(col) < 9) break;
        ++s;
    }
    mask |= 1u << ((col + 10u * cls) & 31u);                        // 0x80065C90
    r.columnClass = static_cast<int32_t>(cls);                      // 0x80065CA8: 0x8006B8A0[handle] = cls
    r.ai = col + 8u * cls + 1u;
    // the record (0x80065CB8..0x80065DA0)
    const uint32_t type = g.U8(gs + 4u);
    uint32_t c = cls;
    const uint32_t p1 = g.U32(kP1Bike);
    if (type == 33u) {
        c = g.U32(p1 + 180u) < 9u ? 1u : 0u;
    } else if ((type & 1u) && !(g.S32(kBikeCount) < 3)) {
        const uint32_t pc = g.U8(g.U32(p1 + 1084u) + 1u) & 0xFu;
        const uint32_t which = pc == 2u ? 1u : (g.U32(gs + 48u) < 2u ? 1u : 0u);
        c = g.U8(g.U32(g.U32(0x8005B3A0u) + 1096u * which + 1084u) + 1u) & 0xFu;
    } else if ((type & 4u) && type != 44u) {
        c = g.U8(g.U32(p1 + 1084u) + 1u) & 0xFu;
    }
    r.record = col + 8u * c + 1u;
    return r;
}

void SpawnCopBike(rr::sim::GuestRam& g, uint32_t e) {
    const uint32_t R = g.U32(e + 852u);
    g.W8(e + 0x3B2u, 1);
    g.W32(e + 0x3B4u, 0);
    g.W32(e + 0x3B8u, 0);
    g.W16(e + 0x3BCu, 1);                                           // op 1 (0x80066C10..28)
    g.W16(e + 0x3BEu, 224);
    g.W32(e + 0xB0u, 0xFFFFFFFFu);                                  // 0x80066D44 / 0x80066D48
    g.W32(R + 0xB0u, 0xFFFFFFFFu);
    g.W16(R + 0x220u, 224);
    g.W8(R + 0x239u, 41);
    g.W8(R + 0x23Au, 0);
    g.W8(R + 0x23Bu, 0xFF);
    g.W8(R + 0x23Cu, 0);
    g.W32(R + 0x22Cu, 0);
    g.W32(R + 0x25Cu, 0);                                           // race type bit 0 clear
    g.W16(e + 0x140u, 0);                                           // 0x80066DD8
    g.W8(e + 0x3A0u, static_cast<uint8_t>(g.U8(e + 0x3A0u) | 0x30u));
    g.W16(e + 0x366u, 0);                                           // 0x80066DE4 (every bike)
}

void SaveCopRecord(rr::sim::GuestRam& g, uint32_t e) {
    if (e == 0) return;
    const uint32_t gs = g.U32(kGsPtr);
    if (g.U16(e + 172u) < g.U32(gs + 48u)) return;
    const uint32_t rd = g.U32(e + 1084u);
    if ((g.U8(rd + 1u) & 0xFu) != 2u) return;
    constexpr uint32_t rec = 0x800D86F0u;
    g.W8(rec + 19u, 1);
    g.W8(rec + 12u, g.U8(rd + 12u));
    g.W8(rec + 13u, g.U8(rd + 13u));
    g.W8(rec + 15u, g.U8(rd + 15u));
    g.W8(rec + 14u, g.U8(rd + 14u));
    g.W8(rec + 16u, g.U8(rd + 2u));
    g.W8(rec + 17u, g.U8(rd + 61u));
    g.W8(rec + 18u, g.U8(rd + 36u));
    g.W8(rec + 22u, g.U8(rd + 46u));
    g.W16(rec + 20u, g.U16(rd + 44u));
    const uint32_t stat = g.U32(g.U32(e + 556u) + 224u);            // stats[+0xE0]
    g.W32(rec + 24u, 0);
    g.W32(rec + 8u, stat);
}

void PoliceCensus(rr::sim::GuestRam& g) {
    constexpr uint32_t pool = 0x800CE4D0u;
    int32_t n = g.S32(g.U32(pool + 12u));
    uint32_t e = g.U32(pool);
    uint32_t all = 0, live = 0;
    while (n >= 0) {
        if ((g.U8(g.U32(e + 1084u) + 1u) & 0xFu) == 2u) {
            ++all;
            if (g.S16(e + 320u) != 0) ++live;
        }
        e += g.U32(pool + 4u);
        --n;
    }
    g.W32(0x800D86F4u, all);
    g.W32(0x800D86F0u, live);
}

namespace {
void SetBits12(rr::sim::GuestRam& g, uint32_t a, uint32_t v) {
    g.W32(a, (g.U32(a) & 0xFFFC0FFFu) | ((v & 0x3Fu) << 12));
}
} // namespace

bool LoadRiderRecords(rr::sim::GuestRam& g, const std::vector<uint8_t>& bi, uint32_t biAt,
                      std::vector<RiderSeat>& seats, std::string& note) {
    namespace s = rr::sim;
    if (bi.size() < kLevelBiBytes || seats.size() > 18u) {
        note = "the LEVEL<n>.BI file is missing or short, or the grid has more than 18 bikes: the rider records are "
               "NOT loaded";
        return false;
    }
    uint8_t saved[24];
    g.ReadBlock(s::kRrSpawnClassTable, saved, sizeof(saved));
    g.WriteBlock(biAt, bi.data(), kLevelBiBytes);
    for (uint32_t k = 0; k < 20u; ++k) g.W8(s::kRrAiToHandle + k, 31);   // 0x80067B8C (memset 20)
    for (uint32_t h = 0; h < 18u; ++h) g.W8(s::kRrHandleToAi + h, 31);   // 0x80067BA0 (byte fill 18)
    uint32_t mask = 0;                                                   // BuildGrid's sp+2304
    bool ok = true;
    std::string order;
    for (RiderSeat& st : seats) {
        const uint32_t e = st.entity;
        const uint32_t h = g.U16(e + 172u);
        const SpawnIndex si = SpawnAiIndex(g, st.slot, st.player, mask);
        if (!(si.ai < 20u) || !(si.record < 27u) || !(h < 18u)) {
            note = "SpawnBike's rule gave an AI index / record / handle out of range: the rider records are NOT loaded";
            ok = false;
            break;
        }
        if (si.columnClass >= 0) g.W8(s::kRrSpawnClassTable + h, static_cast<uint8_t>(si.columnClass));
        g.W8(s::kRrHandleToAi + h, static_cast<uint8_t>(si.ai));             // 0x80065DC4
        g.W8(s::kRrAiToHandle + si.ai, static_cast<uint8_t>(h));             // 0x80065DD0
        const uint32_t rd = s::kRiderRecords + s::kRiderRecordBytes * si.ai;
        g.W32(e + 1084u, rd);                                                 // 0x80065DF0
        s::RiderRecordInit(g, biAt, static_cast<int32_t>(si.record), e);     // 0x80065DEC
        const uint32_t gs = g.U32(s::kRrGameStatePtr);
        const uint32_t type = g.U8(gs + 4u);
        if (type == 33u || type == 44u) {                                     // 0x80065DF4..0x80065EA4
            const uint32_t p1 = g.U32(s::kRrP1Bike);
            if ((g.U8(g.U32(e + 1084u) + 1u) & 0xFu) != (g.U8(g.U32(p1 + 1084u) + 1u) & 0xFu)) {
                const uint32_t R = g.U32(e + 852u);
                SetBits12(g, e + 36u, g.U8(biAt + 1206u));
                SetBits12(g, R + 36u, g.U8(biAt + 1207u));
                rr::sim::GuestCopyWords(g, e + 76u, biAt + 1208u, 4u);
                rr::sim::GuestCopyWords(g, g.U32(e + 852u) + 76u, biAt + 1212u, 4u);
            }
        }
        g.W8(g.U32(e + 1084u) + 38u, static_cast<uint8_t>(s::RiderNameId(g, e)));   // 0x80065EB8
        if (g.U8(g.U32(s::kRrGameStatePtr) + 4u) & 8u) {                      // 0x80065F64..0x80065FC4
            const uint32_t r = g.U32(e + 1084u);
            g.W16(r + 44u, static_cast<uint16_t>(g.U16(r + 44u) & 0xFECDu));
            const uint32_t w = g.U8(g.U32(e + 1084u) + 46u);
            if (w - 4u < 2u || w == 1u || w == 8u) {
                g.W8(g.U32(e + 1084u) + 46u, 9);
                g.W8(g.U32(e + 1084u) + 47u, 0);
            }
        }
        if (si.riderDef1 >= 0) g.W8(g.U32(e + 1084u) + 1u, static_cast<uint8_t>(si.riderDef1)); // 0x80066008..20
        RiderModelClass(g, e); // SpawnBike 0x80066350..0x800663FC: the rider's +0xB4 (rider_model.h)
        SpawnBikeClass(g, e, st.player); // SpawnBike 0x800662F0..0x80066328: the bike's +0xB4 (world_pop_product.h)
        {                                                                     // 0x80066634..0x80066674
            const uint32_t on = (st.player >= 0 && !(g.U8(g.U32(s::kRrGameStatePtr) + 4u) & 1u)) ? 0x60u : 0u;
            const uint32_t r = g.U32(e + 1084u);
            g.W8(r, static_cast<uint8_t>(g.U8(r) | on));
        }
        {
            const uint32_t gs2 = g.U32(s::kRrGameStatePtr);
            if (g.U8(gs2 + 4u) == 33u && h < g.U32(gs2 + 48u)) {              // 0x80066678..0x800666C0
                const uint32_t r = g.U32(e + 1084u);
                g.W8(r + 1u, static_cast<uint8_t>((g.U8(r + 1u) & 0xF0u) | 2u));
            }
            const uint32_t r = g.U32(e + 1084u);                              // 0x80066D38..0x80066D90
            if (g.U8(gs2 + 4u) != 33u && (g.U8(r + 1u) & 0xFu) == 2u && (g.U16(r + 44u) & 4u)) g.W8(r + 46u, 2);
        }
        st.ai = si.ai;
        st.record = si.record;
        st.riderDef = g.U32(e + 1084u);
        char b[32];
        std::snprintf(b, sizeof(b), "%s%u/%u", order.empty() ? "" : ", ", si.ai, si.record);
        order += b;
    }
    if (ok)
        for (const RiderSeat& st : seats) s::GridRiderAdjust(g, st.entity);  // 0x80068240
    g.WriteBlock(s::kRrSpawnClassTable, saved, sizeof(saved));
    for (uint32_t k = 0; k < kLevelBiBytes; k += 4) g.W32(biAt + k, 0);
    if (g.Faulted()) {
        note = "the rider-record loader met an address the console would fault on";
        return false;
    }
    if (ok)
        note = "the rider records are BuildGrid's (RiderRecordInit 0x80064C0C, RiderNameId 0x80066E1C, "
               "GridRiderAdjust 0x800650A0 PORTED; SpawnBike's record stores transcribed; AI index/record: " +
               order + "); NOT ported: a player's appearance 0x8005DD9C";
    return ok;
}

bool CheckRiderRecords(const DiscImage& disc, const std::string& path, bool mutate, std::string& report) {
    namespace fs = std::filesystem;
    namespace s = rr::sim;
    constexpr uint32_t kGp = 0x8005AC8C;     // SLUS_010.53's gp (race_session.cpp kArenaGp)
    constexpr uint32_t kBiAt = 0x801FE7C8;   // the product's placement (race_session.cpp); stack in a capture
    std::vector<std::string> images;
    std::error_code ec;
    if (fs::is_directory(path, ec)) {
        for (const auto& d : fs::recursive_directory_iterator(path, ec))
            if (d.is_regular_file() && (d.path().filename() == "ram.bin" ||
                                        (d.path().parent_path() == fs::path(path) && d.path().extension() == ".bin")))
                images.push_back(d.path().string());
    } else {
        images.push_back(path);
    }
    std::sort(images.begin(), images.end());
    size_t pass = 0, fail = 0, refused = 0, totalBytes = 0;
    report.clear();
    for (const std::string& p : images) {
        std::ifstream in(p, std::ios::binary);
        std::vector<uint8_t> A((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (A.size() < s::GuestRam::kRamSize) continue;
        s::GuestRam ga(A.data(), kGp);
        auto refuse = [&](const char* why) {
            ++refused;
            report += "  refused " + p + ": " + why + "\n";
        };
        const uint32_t gs = ga.U32(s::kRrGameStatePtr);
        const uint32_t pool = ga.U32(s::kRrPool0Ptr);
        const uint32_t n = ga.U32(0x8005B1F8u);
        if (gs < 0x80000000u || gs >= 0x80200000u || pool < 0x80000000u || pool >= 0x80200000u || n == 0u || n > 18u) {
            refuse("no race in this image (game_state / pool 0 / bike count)");
            continue;
        }
        bool sane = true;
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t rd = ga.U32(pool + 1096u * i + 1084u);
            if (rd < s::kRiderRecords || rd >= s::kRiderRecords + 20u * 72u || (rd - s::kRiderRecords) % 72u != 0u ||
                ga.U16(pool + 1096u * i + 172u) != i)
                sane = false;
        }
        const uint32_t type = ga.U8(gs + 4u);
        if (!sane) {
            refuse("no race in this image (the bikes' rider-record pointers / handles)");
            continue;
        }
        if (type == 17u || (type & 0xCu)) {
            refuse("a race type whose grid BuildGrid reorders (17, & 4, & 8: 0x80067E58..0x800680DC, not transcribed)");
            continue;
        }
        const uint32_t players = ga.U32(gs + 48u);
        const int32_t bank = ga.S32(gs + 60u);
        const int32_t raceId = ga.S32(gs + 0x40u);
        std::vector<uint8_t> bi;
        if (bank >= 0 && bank < 3)
            if (const auto f = disc.Find("DATA/LEVEL" + std::to_string(bank + 1) + ".BI")) bi = disc.ReadFile(*f);
        std::vector<StartEntry> grid;
        std::string why;
        if (bi.size() < kLevelBiBytes || !ReadStartBlock(disc, players >= 2u ? 2 : 1, raceId, grid, why) ||
            grid.size() < n) {
            ++fail;
            report += "  DIFFER " + p + ": no LEVEL<n>.BI / start block for this race (" + why + ")\n";
            continue;
        }
        if (mutate) {                                        // the control: every record one out of step
            bi.erase(bi.begin(), bi.begin() + 64);
            bi.resize(kLevelBiBytes, 0);
        }
        // B: the image with everything the loader writes wiped, and the AI indices the image holds
        std::vector<uint8_t> B = A;
        s::GuestRam gb(B.data(), kGp);
        std::vector<bool> spawned(20, false);
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t e = pool + 1096u * i, R = ga.U32(e + 852u);
            spawned[(ga.U32(e + 1084u) - s::kRiderRecords) / 72u] = true;
            gb.W32(e + 1084u, 0);
            gb.W32(e + 36u, ga.U32(e + 36u) & 0xFFFC0FFFu);
            gb.W32(e + 76u, 0);
            gb.W32(R + 36u, ga.U32(R + 36u) & 0xFFFC0FFFu);
            gb.W32(R + 76u, 0);
            gb.W32(R + 316u, 0);
            gb.W32(R + 180u, 0xA5A5A5A5u); // the rider's model class, SpawnBike 0x800663FC (rider_model.h)
            gb.W32(e + 180u, 0xA5A5A5A5u); // the bike's class, SpawnBike 0x80066328 (world_pop_product.h)
        }
        for (uint32_t k = 0; k < 20u; ++k)
            if (spawned[k])
                for (uint32_t o = 0; o < 72u; ++o) gb.W8(s::kRiderRecords + 72u * k + o, 0xA5);
        for (uint32_t k = 0; k < 20u; ++k) gb.W8(s::kRrAiToHandle + k, 0xA5);
        for (uint32_t h = 0; h < 18u; ++h) gb.W8(s::kRrHandleToAi + h, 0xA5);
        std::vector<RiderSeat> seats;
        for (uint32_t i = 0; i < n; ++i) {
            RiderSeat st;
            st.entity = pool + 1096u * i;
            st.slot = grid[i].slot;
            st.player = i == 0 ? 0 : (players == 2u && i == 1u ? 1 : -1);
            seats.push_back(st);
        }
        std::string note;
        const bool loaded = LoadRiderRecords(gb, bi, kBiAt, seats, note);
        // compare where the loader's value survives the race
        size_t compared = 0, differ = 0;
        std::string first;
        auto cmp = [&](uint32_t a, uint32_t bytes, uint32_t maskBits, uint32_t bike, const char* what) {
            for (uint32_t k = 0; k < bytes; ++k) {
                const uint32_t x = ga.U8(a + k) & maskBits, y = gb.U8(a + k) & maskBits;
                ++compared;
                if (x != y) {
                    ++differ;
                    if (first.empty()) {
                        char b[160];
                        std::snprintf(b, sizeof(b), "bike %u %s +%u: image 0x%02X, loader 0x%02X", bike, what, k, x, y);
                        first = b;
                    }
                }
            }
        };
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t e = pool + 1096u * i, R = ga.U32(e + 852u), rd = ga.U32(e + 1084u);
            const uint32_t ai = (rd - s::kRiderRecords) / 72u;
            const bool player = i < players;
            const bool cop = !player && (ga.U8(rd + 1u) & 0xFu) == 2u;
            cmp(e + 1084u, 4, 0xFF, i, "bike +0x43C");
            cmp(R + 180u, 4, 0xFF, i, "rider +0xB4 (model class)"); // rider_model.h
            cmp(e + 180u, 4, 0xFF, i, "bike +0xB4 (class)"); // world_pop_product.h
            cmp(s::kRrHandleToAi + i, 1, 0xFF, i, "handle->AI");
            cmp(s::kRrAiToHandle + ai, 1, 0xFF, i, "AI->handle");
            cmp(rd + 0x01u, 1, 0xFF, i, "rd+0x01");
            cmp(rd + 0x02u, 1, 0xF0, i, "rd+0x02 (resting mood)");
            cmp(rd + 0x03u, 1, 0xFF, i, "rd+0x03");
            cmp(rd + 0x08u, 7, 0xFF, i, "rd+0x08..0x0E");
            for (uint32_t k = 0; k < 20u; ++k)
                if (!(k < players)) cmp(rd + 0x10u + k, 1, 0xFF, i, "rd grudge");
            cmp(rd + 0x24u, 1, 0xFF, i, "rd+0x24");
            if (!cop) cmp(rd + 0x26u, 1, 0xFF, i, "rd+0x26 (name)");
            cmp(rd + 0x30u, 12, 0xFF, i, "rd+0x30..0x3B");
            cmp(rd + 0x45u, 3, 0xFF, i, "rd+0x45..0x47");
            cmp(R + 316u, 4, 0xFF, i, "rider +0x13C");
            cmp(R + 76u, 4, 0xFF, i, "rider +0x4C");
            if (!player) {
                cmp(e + 37u, 1, 0xF0, i, "bike +0x24 bits 12..15");
                cmp(e + 38u, 1, 0x03, i, "bike +0x24 bits 16..17");
                cmp(R + 37u, 1, 0xF0, i, "rider +0x24 bits 12..15");
                cmp(R + 38u, 1, 0x03, i, "rider +0x24 bits 16..17");
            }
        }
        totalBytes += compared;
        const bool same = loaded && differ == 0;
        (same ? pass : fail) += 1;
        char line[200];
        std::snprintf(line, sizeof(line), "race type %u, bank %d, race %d, %u bikes: %zu byte(s) compared, %zu differ",
                      type, bank, raceId, n, compared, differ);
        report += (same ? "  same    " : "  DIFFER  ") + p + ": " + line + (first.empty() ? "" : " (first: " + first + ")") +
                  (loaded ? "" : " - loader: " + note) + "\n";
    }
    report += "ridercheck" + std::string(mutate ? "-mutate" : "") + ": " + std::to_string(pass) + " of " +
              std::to_string(pass + fail) + " race image(s) hold the rider records the ported loader builds (" +
              std::to_string(totalBytes) + " bytes compared), " + std::to_string(refused) + " refused";
    return fail == 0 && pass > 0;
}

} // namespace rr::game
