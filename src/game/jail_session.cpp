// The Jailbreak mode and the two-seat bike in the product (jail_session.h, docs\formats\rules.md 16).
#include "game/jail_session.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "game/race_session.h"
#include "game/loader_product.h"   // EscapeLoad PORTED in BuildRace
#include "game/sim/traffic_bind.h" // ModelBind SLUS 0x8002FAD4 (SpawnPassenger)
#include "game/rider_model.h"       // SetRiderBoxes: SLUS 0x80012FC8 on the rider's model
#include "game/rider_pose.h"         // look3: kPoseReserveBytes
#include "game/sim/ai.h"          // AiPopCommand / AiPushCommand
#include "game/sim/coll_util.h"
#include "game/sim/jail.h"
#include "game/sim/population.h"    // RoadGate, CursorSeat, Attach
#include "game/sim/recover_walk.h"  // AxisRotation
#include "game/sim/rider_record.h"  // RiderRecordInit
#include "game/sim/road_runtime.h"  // RoadRebindBody (0x80037450)
#include "game/sim/spine.h"         // ResetBikeState SLUS 0x8002090C
#include "game/sim/stance.h"
#include "game/weapon_session.h"    // ProductWeapon: WeaponObject 0x800958F0

namespace rr::game {

namespace {

using rr::sim::GuestRam;
using rr::sim::cu::S;
using rr::sim::cu::U;

constexpr uint32_t kGp = 0x8005AC8C;            // SLUS_010.53's gp (race_session.cpp kArenaGp)
constexpr uint32_t kGsPtr = 0x8005B2F8;
constexpr uint32_t kPool0Base = 0x8005B3A0, kPool1Base = 0x8005B3A4;
constexpr uint32_t kPool0Count = 0x8005B1F8, kPool0High = 0x8005AD38;
constexpr uint32_t kPool1Count = 0x8005B218, kPool1High = 0x8005AD3C;
constexpr uint32_t kPoolBytes = 0x8005B2B8;     // u8 {pool 0 capacity, allocated, pool 1 capacity, allocated}
constexpr uint32_t kPlayer2Bike = 0x8005B21C;
constexpr uint32_t kBikeBytes = 1096, kRiderBytes = 628;
constexpr uint32_t kPoolSlots = 18;             // the arena's pools hold 18 (race_session.cpp's layout)
constexpr uint32_t kBiAt = 0x801FE7C8;          // race_session.cpp kRiderBiAt: LEVEL<n>.BI while records load
constexpr uint32_t kSeamSp = 0x801FE400;        // race_session.cpp kRecoverSeamSp
constexpr uint32_t kArenaGameState = 0x800D5D38; // race_session.cpp: game_state, *(0x8005B2F8)
constexpr uint32_t kArenaPool1 = 0x801BB2EC;     // race_session.cpp: pool 1, stride 628
constexpr uint32_t kArenaObjectsTo = 0x801A7000; // race_session.cpp: the end of the packed object area

uint8_t* Raw(uint8_t* ram, uint32_t a, uint32_t n) {
    if (a < 0x80000000u || static_cast<uint64_t>(a) + n > 0x80200000ull) return nullptr;
    return ram + (a - 0x80000000u);
}

} // namespace

// ============================================================================ the loader's parts

bool SidecarBike(uint32_t bikeIndex) { return (bikeIndex - 6u) < 3u || (bikeIndex - 15u) < 3u; }

int PlayerPaletteBlock(const uint8_t* ram, int p) {
    auto u32 = [ram](uint32_t a) {
        uint32_t v = 0;
        std::memcpy(&v, ram + (a & 0x1FFFFFu), 4);
        return v;
    };
    const uint32_t gs = u32(kGsPtr);
    const int32_t idx = static_cast<int32_t>(u32(gs + 0x48u + 4u * static_cast<uint32_t>(p)));
    const int cls = idx < 9 ? 0 : (static_cast<uint32_t>(idx - 9) < 9u ? 1 : 2);       // 0x8005DC44..0x8005DC60
    if (cls == 2) return -1;
    const int32_t u = u32(gs + 0x30u) == 2u ? p                                         // 0x8005DC84
                                             : static_cast<int8_t>(ram[(0x800D81D8u + 36u * static_cast<uint32_t>(p) + 10u) & 0x1FFFFFu]);
    return cls == 1 ? u * 6 + 3 : u * 6;                                                // 0x8005DC94..0x8005DCB4
}

std::vector<AnimBankName> AnimBankSet(uint32_t type) {
    std::vector<AnimBankName> v;
    v.push_back({"ANIMTBL2", 1});                                       // 0x8006642C
    v.push_back({type == 44u ? "ANIMTBJ3" : "ANIMTBL3", 2});            // 0x80066468
    if (type & 8u) {                                                    // 0x80066484
        v.push_back({"ANIMTBSB", 4});
        v.push_back({"ANIMTBS1", 0});
        v.push_back({"ANIMTBSW", 3});
        v.push_back({type == 44u ? "ANIMTBLJ" : "ANIMTBLS", 6});        // 0x80066500
    } else {
        v.push_back({"ANIMTBLB", 4});
        v.push_back({"ANIMTBL1", 0});
        v.push_back({"ANIMTBLW", 3});
    }
    if ((type & 0x18u) == 0u) v.push_back({"ANIMTBLP", 5});             // 0x80066564: *(0x8005B254)
    return v;
}

std::string TwoRiderGrid(const uint8_t* gs, int players, std::vector<StartEntry>& e) {
    const uint32_t type = gs[4];
    if (type == 17u || !(type & 8u)) return {};                         // 0x80067E6C / 0x80067EF4
    if ((type & 4u) && gs[0x38] == 2u) return {};                       // 0x80067EC4: the police grid
    const int32_t a0 = players == 1 ? -1 : 0;                           // 0x80067F04..0x80067F0C
    const int32_t limit = (type == 44u ? (a0 & 3) : (a0 & 2)) + 10;
    int32_t s5 = static_cast<int32_t>(e.size());
    int32_t s6 = 0;
    for (const StartEntry& x : e) s6 += x.slot >= 17 ? 1 : 0;          // 0x80067E3C..0x80067E44
    const int32_t t3 = (s5 - s6) - limit;                               // 0x80067F30
    if (t3 <= 0) return {};
    const int32_t before = s5;
    s5 -= t3;
    int32_t cnt = players;                                              // *(0x8005B1FC) = players
    int32_t s3 = players;
    bool past = false;
    while (s3 < s5) {                                                   // 0x80067F84
        if (!(cnt < limit)) break;                                      // 0x80067FB8 / 0x80067FD4
        if (e[static_cast<size_t>(s3)].slot < 17) {                     // 0x80067FE4
            size_t j = static_cast<size_t>(s3 + t3);
            while (j < e.size() && e[j].slot >= 17) ++j;                // 0x80067FF8..0x8006801C
            if (j >= e.size()) {                                        // the original reads its stack on
                past = true;
                break;
            }
            e[static_cast<size_t>(s3)].lateral = e[j].lateral;
            ++cnt;
            e[static_cast<size_t>(s3)].along = e[j].along;
        }
        ++s3;
    }
    int32_t to = s3;                                                    // 0x8006806C / the early exit's a0
    for (int32_t k = s3 + 1; k < s5 + t3 && k < before; ++k)            // 0x80068070..0x800680D4
        if (e[static_cast<size_t>(k)].slot >= 17) e[static_cast<size_t>(to++)] = e[static_cast<size_t>(k)];
    e.resize(static_cast<size_t>(s5));
    char b[260];
    std::snprintf(b, sizeof(b),
                  "BuildGrid RASHCDI 0x80067EDC..0x800680D8, the two-rider arm (race type 0x%02X bit 3): %d entries "
                  "- %d police - limit %d = %d dropped, %d kept%s",
                  type, before, s6, limit, t3, s5,
                  past ? " (a racer found no racing entry below it: the original reads its stack there; ours "
                         "stops)" : "");
    return b;
}

// ============================================================================ the session's side

// Before the per-rider set-up (the animation objects, the pose slots, the model binding): which players
// ride a two-seat machine, and the pool slots the passenger's bike and rider will take - the next free
// slot of pool 0 and of pool 1, as AllocBike / AllocRider give them. Player p's bike +0xB4 is ModelBind's
// v0 in SpawnBike (0x80066314: the bike index gs+0x48 + 4p, the class it binds), which BuildGrid's test
// and the camera file (CAMERAS.CA for a sidecar) read.
void RaceSession::JailPlan() {
    passengers_.clear();
    GuestRam g(arena_.Ram(), kGp);
    for (int p = 0; p < players_ && static_cast<size_t>(p) < bikes_.size(); ++p) {
        const uint32_t idx = g.U32(kArenaGameState + 0x48u + 4u * static_cast<uint32_t>(p));
        g.W32(ArenaEntity(static_cast<size_t>(p)) + 180u, idx);          // SpawnBike 0x80066328
        if (!SidecarBike(idx)) continue;                                // BuildGrid 0x80068344 / 0x800683D4
        const size_t n = bikes_.size() + passengers_.size();
        if (n >= kPoolSlots) {
            NoteSeam("the two-seat bike of player " + std::to_string(p + 1) + " has no pool slot left (the arena's "
                     "pools hold 18): its passenger is NOT spawned");
            continue;
        }
        Passenger ps;
        ps.host = static_cast<size_t>(p);
        ps.bike = ArenaEntity(n);
        ps.rider = kArenaPool1 + kRiderBytes * static_cast<uint32_t>(n);
        std::memset(At(ps.bike), 0, kBikeBytes);                        // BuildGrid 0x80067DC8 / 0x80067DD8
        std::memset(At(ps.rider), 0, kRiderBytes);
        passengers_.push_back(ps);
    }
}

// SpawnPassenger RASHCDI 0x800670FC(host, bi, rec, count) for every planned passenger, after the cursors
// are seated (the host's cursor, road position, route binding and box are copied from it), and the
// STARTJBA.BIN reader 0x80068740 for race type 44.
void RaceSession::JailSpawn(const DiscImage& disc) {
    GuestRam g(arena_.Ram(), kGp);
    const uint32_t gs = kArenaGameState;
    const uint32_t type = g.U8(gs + 4u);
    if (!passengers_.empty()) {
        const uint32_t bikesNow = static_cast<uint32_t>(bikes_.size());
        const uint32_t cap = bikesNow + static_cast<uint32_t>(passengers_.size());
        // OURS: the pools' capacity bytes (BuildGrid 0x80067D74 / 0x80067D80 writes its own count) are the
        // grid's bikes plus the passengers, so that AllocBike / AllocRider find room for exactly these and
        // the spawner's pool-0 budget (0x8008CE1C) stays at 0 as before.
        g.W8(kPoolBytes + 0u, static_cast<uint8_t>(cap));
        g.W8(kPoolBytes + 1u, static_cast<uint8_t>(bikesNow));
        g.W8(kPoolBytes + 2u, static_cast<uint8_t>(cap));
        g.W8(kPoolBytes + 3u, static_cast<uint8_t>(bikesNow));
        std::vector<uint8_t> bi;
        const std::string biName = "DATA/LEVEL" + std::to_string(static_cast<int32_t>(g.U32(gs + 60u)) + 1) + ".BI";
        if (const auto f = disc.Find(biName)) bi = disc.ReadFile(*f);
        if (bi.size() < kLevelBiBytes) {
            NoteSeam(biName + " is missing or short: the passengers are NOT spawned");
            passengers_.clear();
        } else {
            g.WriteBlock(kBiAt, bi.data(), kLevelBiBytes);
        }
        std::string line;
        for (Passenger& ps : passengers_) {
            const uint32_t H = bikes_[ps.host].entityAddress;
            const uint32_t count = g.U32(kPool0Count);
            const uint32_t hb4 = g.U32(H + 180u);
            uint32_t rec, a3;
            if (players_ == 1) {                                        // 0x8006838C..0x800683A8
                rec = 1;
                a3 = count;
            } else if (ps.host == 0) {
                rec = hb4 < 9u ? 9u : 17u;
                a3 = count;
            } else {                                                    // player 2: 0x800683FC..0x80068418
                rec = hb4 < 9u ? 8u : 16u;
                a3 = count + 1u;
            }
            // AllocBike 0x80065974(a3): pool 0 slot a3, its handle, the count past it
            if (!(g.S8(kPoolBytes + 1u) < g.S8(kPoolBytes + 0u))) {
                NoteSeam("AllocBike RASHCDI 0x80065974 found pool 0 full: a passenger is NOT spawned");
                continue;
            }
            const uint32_t P = g.U32(kPool0Base) + kBikeBytes * a3;
            if (P != ps.bike) {
                NoteSeam("the passenger's pool-0 slot is not the planned one: it is NOT spawned");
                continue;
            }
            g.W16(P + 172u, static_cast<uint16_t>(a3));
            // 0x800659CC: the count and the high index move only for an index BELOW the count (a0 < 0 is
            // "the next", which is); the passenger's explicit index a3 = the count is not, so pool 0's
            // walks (every pass over 0..*(0x8005AD38)) never visit the passenger's bike - its host does
            if (S(a3) < g.S32(kPool0Count)) {
                g.W32(kPool0Count, g.U32(kPool0Count) + 1u);
                g.W32(kPool0High, a3);
            }
            g.W8(kPoolBytes + 1u, static_cast<uint8_t>(g.U8(kPoolBytes + 1u) + 1u));
            g.W32(H + 856u, P);                                         // 0x80067138
            g.W32(P + 856u, H);
            g.W32(P + 1088u, 0);                                        // no list node
            g.W32(P + 556u, g.U32(H + 556u));                           // the host's stat block
            g.W32(P + 832u, 0);
            g.W32(P + 560u, g.U32(H + 560u));
            if (!rr::sim::ResetBikeState(g, P)) NoteSeam("SLUS 0x8002090C ResetBikeState (PORTED) refused the passenger's bike");
            // AllocRider 0x80065A04(a3)
            if (!(g.S8(kPoolBytes + 3u) < g.S8(kPoolBytes + 2u))) {
                NoteSeam("AllocRider RASHCDI 0x80065A04 found pool 1 full: the passenger's rider is missing");
                continue;
            }
            g.W32(kPool1High, a3);
            const uint32_t R = g.U32(kPool1Base) + kRiderBytes * a3;
            g.W16(R + 172u, static_cast<uint16_t>(a3 + 32u));
            g.W32(kPool1Count, g.U32(kPool1Count) + 1u);
            g.W8(kPoolBytes + 3u, static_cast<uint8_t>(g.U8(kPoolBytes + 3u) + 1u));
            ps.rider = R;
            g.W32(P + 852u, R);                                         // 0x80067188
            g.W32(R + 596u, P);
            g.W32(P + 1084u, rr::sim::kRiderRecords + rr::sim::kRiderRecordBytes * rec);
            g.W8(rr::sim::kRrHandleToAi + g.U16(P + 172u), static_cast<uint8_t>(rec));   // 0x800671AC
            g.W8(rr::sim::kRrAiToHandle + rec, static_cast<uint8_t>(g.U8(P + 172u)));
            const uint32_t hcls = g.U8(g.U32(H + 1084u) + 1u) & 0xFu;
            rr::sim::RiderRecordInit(g, kBiAt, static_cast<int32_t>(hcls + 20u), P);   // 0x800671D0
            g.W32(P + 304u, 29491);                                     // 0x800671E0..0x80067208
            g.W32(P + 308u, 0x8000);
            g.W32(P + 312u, 0x1CCCC);
            g.W16(P + 956u, 1);
            g.W16(P + 958u, 224);
            g.W8(P + 946u, 1);
            g.W32(P + 316u, g.U32(H + 316u));
            if (type == 44u) g.W32(R + 76u, 0x10000);                   // 0x8006722C
            // 0x80067240: the player's appearance 0x8005DD9C (NOT ported, as for the players' own riders)
            const uint32_t cls = hcls * 9u + g.U32(gs + 60u);           // 0x800672A0: ModelBind(R, 1, cls, 1)
            uint32_t bound = cls;
            if (OriginalBind()) { // ModelBind SLUS 0x8002FAD4 PORTED (traffic_bind.h) - the part array on heap 0
                if (!rr::sim::ModelBind(g, R, 1, cls, 1, bound) || g.Faulted()) {
                    g.ClearFault();
                    NoteSeam("SpawnPassenger's ModelBind SLUS 0x8002FAD4 (PORTED) refused the passenger's rider");
                    bound = cls;
                }
            } else {
                g.W8(R + 0x49u, 0xFF);                                  // RegistryBind SLUS 0x8002FF6C: an empty effect chain
            }
            g.W32(R + 180u, bound);
            (void)SetRiderBoxes(g, disc, static_cast<int32_t>(g.U32(gs + 60u)), {R});   // SLUS 0x80012FC8(R, 0)
            g.W8(R + 569u, 41);                                         // 0x800672B8..0x800672F4
            g.W32(R + 604u, 1);
            g.W8(R + 571u, 0xFF);
            g.W32(R + 556u, 0);
            // R + 540 = 0 (0x800672D8): NOT kept - the product's rider has its OURS animation object, as every
            // rider of the grid (race_session.cpp BuildAnimArena)
            g.W8(R + 570u, 0);
            g.W8(R + 572u, 0);
            g.W32(R + 36u, g.U32(R + 36u) | 0x40000u);
            g.W16(R + 544u, 224);
            g.W16(R + 320u, g.U16(H + 320u));
            const uint32_t seat = g.U8(gs + 10u + g.U16(H + 172u));      // 0x80067300: gs+0x0A + the handle
            if (seat != 0u) {
                g.W8(R + 572u, static_cast<uint8_t>(g.U8(R + 572u) | 0x20u));
                const uint32_t hr = g.U32(H + 852u);
                g.W8(hr + 572u, static_cast<uint8_t>(g.U8(hr + 572u) | 0x10u));
                rr::sim::Attach(g, H, R, 2, 1);                         // SLUS 0x80012838(H, R, 2, 1)
            }
            if (seat == 2u) g.W8(R + 572u, static_cast<uint8_t>(g.U8(R + 572u) | 0x40u));
            for (uint32_t k = 0; k < 32u; ++k) g.W8(P + 328u + k, g.U8(H + 328u + k));   // the cursor
            for (uint32_t k = 0; k < 12u; ++k) g.W8(P + 360u + k, g.U8(H + 360u + k));   // the road position
            for (uint32_t k = 0; k < 56u; ++k) g.W8(P + 372u + k, g.U8(H + 372u + k));   // the route binding
            g.W32(P + 324u, g.U32(H + 324u));
            g.W16(P + 320u, g.U16(H + 320u));
            const uint32_t n0 = g.U32(kPool0Count);                     // 0x800673A8: the half extents
            if (!(S(n0) < 3)) {
                for (uint32_t a0 = 2; S(a0) < S(n0); ++a0) {
                    const uint32_t b = g.U32(kPool0Base) + kBikeBytes * a0;
                    // OURS: an opponent's +0xB4 is SpawnBike's class (0x800660D0..0x80066194), which the
                    // product does not store; the test reads it as the loader would have left it
                    uint32_t b4 = g.U32(b + 180u);
                    if (a0 < bikes_.size() && !bikes_[a0].isPlayer) {
                        const uint32_t r1 = g.U8(g.U32(b + 1084u) + 1u);
                        const uint32_t c = r1 & 0xFu;
                        const uint32_t p1c = g.U8(g.U32(bikes_[0].entityAddress + 1084u) + 1u) & 0xFu;
                        const uint32_t s7 = ((type == 33u || type == 44u) && c != p1c) ? 1u : 0u;  // 0x80065E00
                        const uint32_t s1 = s7 != 0u ? 2u : c;                                   // 0x80066138..54
                        const uint32_t s4 = s1 < 2u ? ((r1 >> 4) >= 2u ? 1u : 0u) : 0u;
                        b4 = s1 * 9u + s4 * 3u + g.U32(gs + 60u);
                    }
                    if ((b4 >= 9u) == (hb4 < 9u)) continue;              // 0x80067410
                    g.W32(H + 308u, g.U32(b + 308u));
                    g.W32(H + 304u, g.U32(b + 304u));
                    g.W32(H + 312u, g.U32(b + 312u));
                }
            } else {
                g.W32(H + 308u, 0xC000);
                g.W32(H + 304u, 0x4000);
                g.W32(H + 312u, 0x10000);
            }
            rr::sim::cu::GMulAdd(g, H + 184u, H + 432u, S(g.U32(H + 304u) + g.U32(P + 304u)), P + 184u);   // 0x80067478
            for (uint32_t k = 0; k < 3u; ++k) {
                g.W32(P + 504u + 4u * k, g.U32(P + 184u + 4u * k));
                g.W32(P + 468u + 4u * k, g.U32(P + 184u + 4u * k));
            }
            const int32_t lat = g.S32(P + 344u);                        // 0x800674B4
            g.W32(P + 344u, U(lat > 0 ? S(U(lat) + g.U32(H + 308u)) : S(U(lat) - g.U32(H + 308u))));
            ps.spawned = !g.Faulted();
            char b[200];
            std::snprintf(b, sizeof(b), "%splayer %zu's sidecar (bike %u): passenger bike 0x%08X (pool 0 slot %u), rider "
                          "0x%08X, record %u, %s", line.empty() ? "" : "; ", ps.host + 1, hb4, P, a3, R, rec,
                          seat != 0u ? "seated in the sidecar (gs+0x0A)" : "not seated (picked up in the escape scene)");
            line += b;
        }
        for (uint32_t k = 0; k < kLevelBiBytes; k += 4) g.W32(kBiAt + k, 0);
        if (g.Faulted()) {
            g.ClearFault();
            NoteSeam("SpawnPassenger RASHCDI 0x800670FC (transcribed) met an address the console would fault on");
        }
        if (!line.empty())
            NoteSeam("SpawnPassenger RASHCDI 0x800670FC (transcribed; BuildGrid 0x80068344..0x80068418): " + line);
    }
    // STARTJBA.BIN, RASHCDI 0x80068740: race type 44 only; the file is opened for block 0 and block 1 reads
    // on from where block 0 stopped (the cursor is not reset). Each block: five counts, then the records
    // {packed, lateral, along} into a malloc'd array as {lateral, along, packed}, group by group.
    if (Loader2Totals().buildRaceRan) { // EscapeLoad 0x80068740 PORTED, run by BuildRace (loader2.h)
        if (type != 44u) return;
        const uint32_t n0 = g.U32(rr::sim::kJailBlockCount), n1 = g.U32(rr::sim::kJailBlockCount + 4u);
        char b[260];
        std::snprintf(b, sizeof(b),
                      "STARTJBA.BIN (RASHCDI 0x80068740, PORTED in BuildRace): %s; block 0 %u record(s), block 1 %u at "
                      "0x%08X / 0x%08X (OURS placement: blocks of the session's bump region)",
                      n0 + n1 != 0u ? "read" : "NOT read", n0, n1, g.U32(rr::sim::kJailBlockArray),
                      g.U32(rr::sim::kJailBlockArray + 4u));
        NoteSeam(b);
        return;
    }
    g.W32(rr::sim::kJailBlockArray + 0u, 0);
    g.W32(rr::sim::kJailBlockArray + 4u, 0);
    g.W32(rr::sim::kJailBlockCount + 0u, 0);
    g.W32(rr::sim::kJailBlockCount + 4u, 0);
    if (type != 44u) return;
    std::vector<uint8_t> f;
    if (const auto e = disc.Find("DATA/STARTJBA.BIN")) f = disc.ReadFile(*e);
    auto word = [&](size_t& at, bool& ok) -> uint32_t {
        if (at + 4u > f.size()) {
            ok = false;
            return 0;
        }
        uint32_t v = 0;
        std::memcpy(&v, f.data() + at, 4);
        at += 4;
        return v;
    };
    size_t at = 0;
    bool ok = !f.empty();
    uint32_t totals[2] = {0, 0};
    for (uint32_t blk = 0; blk < 2u && ok; ++blk) {
        uint32_t counts[5];
        uint32_t total = 0;
        for (uint32_t k = 0; k < 5u; ++k) {
            counts[k] = word(at, ok);
            total += counts[k];
        }
        if (!ok) break;
        g.W32(rr::sim::kJailBlockCount + 4u * blk, total);
        if (total != 0u) {                                              // 0x800688D0: malloc(12 total), zeroed
            const uint32_t arr = (arenaFreeFrom_ + 7u) & ~7u;
            if (arr + 12u * total > kArenaObjectsTo) {
                ok = false;
                break;
            }
            arenaFreeFrom_ = arr + 12u * total + 8u;
            g.W32(rr::sim::kJailBlockArray + 4u * blk, arr);
            for (uint32_t k = 0; k < 12u * total; k += 4) g.W32(arr + k, 0);
            uint32_t i = 0;
            for (uint32_t grp = 0; grp < 5u && ok; ++grp)
                for (uint32_t r = 0; r < counts[grp] && ok; ++r, ++i) {
                    const uint32_t packed = word(at, ok), lateral = word(at, ok), along = word(at, ok);
                    g.W32(arr + 12u * i + 8u, packed);
                    g.W32(arr + 12u * i + 0u, lateral);
                    g.W32(arr + 12u * i + 4u, along);
                }
        }
        totals[blk] = total;
    }
    char b[240];
    std::snprintf(b, sizeof(b),
                  "STARTJBA.BIN (RASHCDI 0x80068740, transcribed): %s; block 0 %u record(s), block 1 %u at 0x%08X / "
                  "0x%08X (OURS placement: the loader mallocs them)",
                  ok ? "read" : "NOT read (missing, short, or no room)", totals[0], totals[1],
                  g.U32(rr::sim::kJailBlockArray), g.U32(rr::sim::kJailBlockArray + 4u));
    NoteSeam(b);
}

// ---- the escape scene's callees on the arena
namespace {

class ProductEscape final : public rr::sim::EscapeCallees {
public:
    ProductEscape(RaceSession& s, GuestRam& g, uint8_t* ram, rr::sim::StanceSeams& seams, const rr::sim::BikeTables& t,
                  uint32_t& counter)
        : s_(s), g_(g), ram_(ram), seams_(seams), t_(t), counter_(counter) {}
    bool SeatRelease(uint32_t b, uint32_t r, uint32_t idx, uint32_t) override {   // 0x80068D20, PORTED (anim.h)
        rr::sim::AnimMachine m(g_, seams_);
        m.SeatRelease(b, r, idx);
        return !m.Failed();
    }
    bool Placement(uint32_t e, uint32_t sp) override {                  // 0x8009432C, PORTED (population.h)
        uint32_t v0 = 0;
        return s_.PopCall(0x8009432Cu, e, 0, sp, v0);
    }
    bool RoadGate(uint32_t h, uint32_t key, uint32_t, uint32_t& v0) override {    // SLUS 0x80039DFC, PORTED
        v0 = rr::sim::RoadGate(g_, h, key);
        return !g_.Faulted();
    }
    bool CursorSeat(uint32_t obj, uint32_t key, uint32_t cursor, uint32_t sp, uint32_t& v0) override {   // SLUS 0x8003A700
        v0 = U(rr::sim::CursorSeat(g_, obj, key, cursor, sp));
        if (std::getenv("RRJB_JAIL_TRACE"))
            std::printf("escape: cursor seat obj 0x%08X key road %u along %.1f -> %u\n", obj, g_.U32(key),
                        g_.S32(key + 8u) / 65536.0, v0);
        return !g_.Faulted();
    }
    bool AxisRotation(uint32_t axis, int32_t ang, uint32_t out, uint32_t sp) override {   // SLUS 0x8003FB34, PORTED
        rr::sim::AxisRotation(g_, axis, ang, out, sp, t_);
        return !g_.Faulted();
    }
    bool BuildObb(uint32_t e, uint32_t) override {                      // 0x8008BA18, PORTED (bike.h)
        uint8_t* ep = Raw(ram_, e, 1096);
        if (ep == nullptr) return false;
        int32_t rev = 0;
        if ((g_.U16(e + 0xACu) >> 5) == 0u) {                           // only the pool-0 arm reads the owner
            const uint32_t owner = g_.U32(e + 852u);
            rev = owner != 0u ? g_.S32(owner + 604u) : 0;
        }
        rr::sim::BuildObb(rr::sim::EntityView(ep), rev);
        return !g_.Faulted();
    }
    bool RoadUpdate(uint32_t e, uint32_t sp) override {                 // SLUS 0x80037450, PORTED (road_runtime.h)
        rr::sim::RoadRuntimeNative road;
        rr::sim::RoadRebindBody(g_, e, sp, road);
        return !g_.Faulted();
    }
    bool WeaponObject(uint32_t r, uint32_t side, uint32_t) override {   // 0x800958F0, PORTED (weapon.h)
        ProductWeapon w(g_, seams_, &s_);
        return w.Object(r, side);
    }
    bool GetRCnt(uint32_t, uint32_t, uint32_t& v0) override {           // SLUS 0x80043F00: OURS counter
        counter_ = counter_ * 1103515245u + 12345u;
        v0 = (counter_ >> 16) & 0xFFFFu;
        s_.NoteSeam("the escape scene's GetRCnt SLUS 0x80043F00 (root counter 2, the prisoners' stance rate) is "
                    "answered by a deterministic counter of ours: the hardware timer is not modelled");
        return true;
    }
    bool StanceEvent(uint32_t ev, uint32_t r, uint32_t p, uint32_t) override {    // 0x800C4550, PORTED
        rr::sim::StanceLayer layer(g_, seams_);
        layer.Event(ev, r, p);
        if (std::getenv("RRJB_JAIL_TRACE"))
            std::printf("escape: stance event %u rider 0x%08X mount %d stance %u +0x23C 0x%02X failed %d fault %d\n", ev, r,
                        g_.S8(r + 72u), g_.U16(r + 544u), g_.U8(r + 572u), layer.Failed() ? 1 : 0, g_.Faulted() ? 1 : 0);
        return !layer.Failed() && !g_.Faulted();
    }
    bool CarSpawn(uint32_t rec, uint32_t a1, uint32_t sp, uint32_t& v0) override {   // 0x8009AD48, PORTED
        return s_.PopCall(0x8009AD48u, rec, a1, sp, v0);
    }
    bool Roadblock(uint32_t rec, uint32_t player, uint32_t sp) override {   // 0x800A2630 PropAlloc4, PORTED (world_pop.h)
        uint32_t v0 = 0;
        const bool ok = s_.PopCall(0x800A2630u, rec, player, sp, v0);
        char b[200];
        std::snprintf(b, sizeof(b), "RASHCDG 0x800A2630 PropAlloc4 (PORTED, world_pop.h) placed a prop of the escape "
                      "scene (group 4 of the STARTJBA.BIN block): record 0x%08X -> entity 0x%08X", rec, v0);
        s_.NoteSeam(b);
        return ok;
    }

private:
    RaceSession& s_;
    GuestRam& g_;
    uint8_t* ram_;
    rr::sim::StanceSeams& seams_;
    const rr::sim::BikeTables& t_;
    uint32_t& counter_;
};

} // namespace

bool RaceSession::ModeEscapeScene(int32_t block, uint32_t sp) {
    GuestRam g(arena_.Ram(), kGp);
    g.SetScratchpad(scratchpad_.data());
    if (modeSeams_ == nullptr) {
        NoteSeam("RASHCDG 0x800C9420 EscapeScene (PORTED) was not run: no rider seams this frame");
        return false;
    }
    const rr::sim::BikeTables t = SessionTables();
    ProductEscape c(*this, g, arena_.Ram(), *modeSeams_, t, jailCounter_);
    ++escapeScenes_;
    const bool ok = rr::sim::EscapeScene(g, block, sp != 0u ? sp : kSeamSp, c) && !g.Faulted();
    if (!ok) {
        g.ClearFault();
        NoteSeam("RASHCDG 0x800C9420 EscapeScene (PORTED, sim\\jail.h) refused: a callee failed or an address faulted");
    }
    char b[200];
    std::snprintf(b, sizeof(b),
                  "RASHCDG 0x800C9420 EscapeScene (PORTED, sim\\jail.h) staged STARTJBA.BIN block %d (%s): %d record(s)",
                  block, block != 0 ? "in time: the jail" : "late: the police", g.S32(rr::sim::kJailBlockCount + 4u * U(block)));
    NoteSeam(b);
    for (const Passenger& ps : passengers_) {
        const uint32_t R = g.U32(ps.bike + 852u);
        char d[300];
        std::snprintf(d, sizeof(d), "  the escape scene left the passenger: bike 0x%08X depth %u top op %u, rider 0x%08X "
                      "mount %d stance %u +0x23C 0x%02X at (%.1f, %.1f, %.1f)", ps.bike, g.U8(ps.bike + 946u),
                      g.U16(ps.bike + 956u + 8u * (g.U8(ps.bike + 946u) - 1u)), R, g.S8(R + 72u), g.U16(R + 544u),
                      g.U8(R + 572u), g.S32(R + 184u) / 65536.0, g.S32(R + 188u) / 65536.0, g.S32(R + 192u) / 65536.0);
        NoteSeam(d);
    }
    {   // the pool-0 bikes the scene's walks choose from: index:class (riderDef +1 & 0xF), handle +0xAC, live +0x140
        std::string l = "  pool 0 at the escape scene (count " + std::to_string(g.U32(kPool0Count)) + ", pool-1 count " +
                        std::to_string(g.U16(kPool1Count)) + "):";
        for (uint32_t i = 0; i < kPoolSlots; ++i) {
            const uint32_t e = g.U32(kPool0Base) + kBikeBytes * i;
            const uint32_t rd = g.U32(e + 1084u);
            char d[64];
            std::snprintf(d, sizeof(d), " %u:c%u/h%u/l%d", i, rd != 0u ? (g.U8(rd + 1u) & 0xFu) : 99u, g.U16(e + 172u),
                          g.S16(e + 320u));
            l += d;
        }
        NoteSeam(l);
    }
    return ok;
}

// A player's sidecar bike is bound to its rig, model 100 + index (LoadBikeBank's .MRO, model_runtime.cpp), whose
// group 0 has six parts; RegistryBind SLUS 0x8002FDEC mallocs the part array as group 0's part count x 24
// (0x8002FE5C: DOD3 +0x18 x 0x18) and starts every slot at the identity (0x8002FF98..0x80030038). The pose
// arena's array for a bike (rider_pose.cpp, rr-race's addresses) holds five, with the rider's slots 0x80 bytes
// on, so the rig's gets its own block here (OURS placement: the session's bump region), before BindModels'
// LodSelect writes the six DPD3 words. BikeWheels 0x80066EC4 then fills slot 5 (the sidecar wheel).
void RaceSession::JailRigParts() {
    GuestRam g(arena_.Ram(), kGp);
    for (const Passenger& ps : passengers_) {
        const uint32_t H = bikes_[ps.host].entityAddress;
        const uint32_t id = 100u + g.U32(H + 180u);
        uint32_t reg = 0;
        for (uint32_t k = 0; k < 50u; ++k)
            if (g.U32(0x800CE1B0u + 16u * k) == id) reg = 0x800CE1B0u + 16u * k;
        if (reg == 0u) {
            NoteSeam("the sidecar rig model " + std::to_string(id) + " is not in the model arena: its bike keeps the "
                     "pose arena's five part slots");
            continue;
        }
        const uint32_t parts = g.U16(g.U32(g.U32(reg + 8u)) + 24u);
        if (OriginalBind() && g.U32(H + 0x60u) == reg && g.U32(H + 4u) != 0u) { // RegistryBind's own array
            char k[220];
            std::snprintf(k, sizeof(k), "the sidecar rig model %u: player %zu's bike has its own %u part slots at 0x%08X "
                          "(RegistryBind SLUS 0x8002FDEC's malloc on heap 0, PORTED)", id, ps.host + 1, parts,
                          g.U32(H + 4u));
            NoteSeam(k);
            continue;
        }
        const uint32_t at = (arenaFreeFrom_ + 7u) & ~7u;
        if (parts == 0u || at + 24u * parts > kArenaObjectsTo) continue;
        arenaFreeFrom_ = at + 24u * parts + 8u;
        for (uint32_t s = 0; s < parts; ++s) {
            const uint32_t a = at + 24u * s;
            for (uint32_t k = 0; k < 24u; k += 4) g.W32(a + k, 0);
            g.W16(a + 4u, 0x1000);
            g.W16(a + 12u, 0x1000);
            g.W16(a + 20u, 0x1000);
        }
        g.W32(H + 4u, at);
        char b[200];
        std::snprintf(b, sizeof(b), "the sidecar rig model %u: player %zu's bike has its own %u part slots at 0x%08X "
                      "(RegistryBind SLUS 0x8002FDEC's malloc; OURS placement)", id, ps.host + 1, parts, at);
        NoteSeam(b);
    }
}

} // namespace rr::game

// ---- the jail stop (the AI drive's op 2 in phases 1 / 2) and the boarding
namespace rr::game {
namespace {

constexpr uint32_t kAltKind = 0x800541D4, kFightPtr = 0x8005AD4C, kAttackers = 0x800CCAC0;

// AiPopCommand 0x800BC8DC and AiPushCommand 0x800BCA68 (PORTED, ai.h) on raw views of the arena, their idle
// stance through the session's PORTED stance event.
class ProductJail final : public rr::sim::JailCallees {
public:
    ProductJail(GuestRam& g, uint8_t* ram, rr::sim::StanceSeams& seams) : g_(g), ram_(ram), seams_(seams) {}
    bool PopCommand(uint32_t e, uint32_t) override {
        uint8_t* ep = Raw(ram_, e, 1096);
        const uint32_t R = g_.U32(e + 852u);
        Sink sink(*this);
        static const std::vector<uint8_t> noFight(12u * 256u, 0);
        rr::sim::AiPopEnv pe;
        pe.gameState = Raw(ram_, g_.U32(kGsPtr), 64);
        pe.riderDef = Raw(ram_, g_.U32(e + 1084u), 72);
        pe.rider = Raw(ram_, R, 640);
        pe.riderAddress = R;
        pe.altKindTable = Raw(ram_, kAltKind, 8u * 65536u);
        const uint32_t fp = g_.U32(kFightPtr);
        pe.fightRecords = fp != 0u ? Raw(ram_, fp, 12u * 256u) : noFight.data();
        pe.attackerMask = reinterpret_cast<const uint16_t*>(Raw(ram_, kAttackers, 16));
        pe.stance = &sink;
        if (ep == nullptr || pe.gameState == nullptr || pe.riderDef == nullptr || pe.altKindTable == nullptr) return false;
        rr::sim::AiPopCommand(ep, pe);
        return sink.ok && !g_.Faulted();
    }
    bool PushCommand(uint32_t cmd, int32_t mode, uint32_t e, uint32_t) override {
        uint8_t* ep = Raw(ram_, e, 1096);
        uint8_t* rec = Raw(ram_, cmd, 8);
        const uint32_t R = g_.U32(e + 852u);
        const uint32_t fp = g_.U32(kFightPtr);
        static const std::vector<uint8_t> noFight(12u * 256u, 0);
        if (ep == nullptr || rec == nullptr) return false;
        Sink sink(*this);
        rr::sim::AiPushEnv pe;
        pe.raceClock = g_.S32(g_.U32(kGsPtr) + 0x10u);
        pe.rider = Raw(ram_, R, 640);
        pe.riderAddress = R;
        pe.altKindTable = Raw(ram_, kAltKind, 8u * 256u);
        pe.fightRecords = fp != 0u ? Raw(ram_, fp, 12u * 256u) : noFight.data();
        pe.stance = &sink;
        uint8_t c[8];
        std::memcpy(c, rec, 8);
        rr::sim::AiPushCommand(c, mode, ep, pe);
        std::memcpy(rec, c, 8);
        return sink.ok && !g_.Faulted();
    }

private:
    struct Sink final : rr::sim::AiStanceSink {
        ProductJail& j;
        bool ok = true;
        explicit Sink(ProductJail& x) : j(x) {}
        void PlayIdleStance(uint16_t event, uint32_t rider) override {
            rr::sim::StanceLayer layer(j.g_, j.seams_);
            layer.Event(event, rider, 2);
            if (layer.Failed() || j.g_.Faulted()) ok = false;
        }
    };
    GuestRam& g_;
    uint8_t* ram_;
    rr::sim::StanceSeams& seams_;
};

} // namespace

bool RaceSession::ModeJailFinish(uint32_t sp) {
    GuestRam g(arena_.Ram(), kGp);
    if (modeSeams_ == nullptr) return false;
    ProductJail c(g, arena_.Ram(), *modeSeams_);
    const rr::sim::BikeTables t = SessionTables();
    ++jailStops_;
    const bool ok = rr::sim::JailbreakFinish(g, sp, t, c) && !g.Faulted();
    if (!ok) {
        g.ClearFault();
        NoteSeam("RASHCDG 0x800C9E74 JailbreakFinish (PORTED, sim/jail.h) refused a frame");
    }
    return ok;
}

bool RaceSession::ModeJailBoard(uint32_t late, uint32_t sp) {
    GuestRam g(arena_.Ram(), kGp);
    if (modeSeams_ == nullptr) return false;
    ProductJail c(g, arena_.Ram(), *modeSeams_);
    const rr::sim::BikeTables t = SessionTables();
    ++jailBoards_;
    const bool ok = rr::sim::JailBoard(g, late, sp, t, c) && !g.Faulted();
    if (!ok) {
        g.ClearFault();
        NoteSeam("RASHCDG 0x800CA05C JailBoard (PORTED, sim/jail.h) refused a call");
    }
    return ok;
}

} // namespace rr::game

namespace rr::game {

// The short animation set of a two-seat race (AnimBankSet) is ~25 KB larger than the long one and no longer
// ends below the pose arena's part arrays at rr-race's addresses (rider_pose.h kPairParts); OURS: the
// passenger bank (stance bank 6, ANIMTBLS / ANIMTBLJ) takes its room in the object area here, before the
// scene cells' buffers are carved out of what is left (the loader mallocs every bank; no capture has one).
void RaceSession::JailReserve(const DiscImage& disc) {
    jailBankAt_ = 0;
    posePairsAt_ = 0;
    const uint32_t type = gameState_[4];
    if (!(type & 8u)) return;
    // look3: the whole short set, the passenger bank with the others, fits the bank area [0x801BE13C, 0x801EE000)
    // (191,604 B of files for Side Car, 190,988 for Jailbreak, plus their clip tables) but not with rr-race's pose
    // part arrays at 0x801E7F3C in it; those (~10 KB) take their room here instead of the bank (~66 KB), which left
    // the object area no room for the frame's packet heap or the sound state's block. RRJB_JAIL_BANK=object: the
    // passenger bank here (the control).
    if (std::getenv("RRJB_JAIL_BANK") == nullptr || std::strcmp(std::getenv("RRJB_JAIL_BANK"), "object") != 0) {
        const uint32_t at = (arenaFreeFrom_ + 15u) & ~15u;
        if (at + kPoseReserveBytes <= kArenaObjectsTo) {
            posePairsAt_ = at;
            arenaFreeFrom_ = at + kPoseReserveBytes;
            char b[200];
            std::snprintf(b, sizeof(b), "the pose arena's part arrays of the two-seat race take 0x%08X..0x%08X of the object "
                          "area (OURS); the passenger stance bank loads with the others", at, arenaFreeFrom_);
            NoteSeam(b);
            return;
        }
    }
    for (const AnimBankName& bf : AnimBankSet(type)) {
        if (bf.stanceBank != 6u) continue;
        const auto e = disc.Find(std::string("DATA/") + bf.name + ".PSX");
        if (!e) return;
        const uint32_t size = static_cast<uint32_t>(e->size);
        const uint32_t need = ((size + 8u) & ~7u) + ((size / 64u + 64u) & ~7u) + 64u; // the file and its clip table
        const uint32_t at = (arenaFreeFrom_ + 7u) & ~7u;
        if (at + need > kArenaObjectsTo) {
            NoteSeam(std::string("the object area has no room for ") + bf.name + ".PSX: it is loaded with the others");
            return;
        }
        jailBankAt_ = at;
        arenaFreeFrom_ = at + need;
        char b[200];
        std::snprintf(b, sizeof(b), "the passenger stance bank %s.PSX (bank 6 of the short set) is placed at 0x%08X "
                      "in the object area (OURS: the loader mallocs it)", bf.name, at);
        NoteSeam(b);
    }
}

} // namespace rr::game
