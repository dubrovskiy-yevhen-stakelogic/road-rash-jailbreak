#include "game/sim/view_pass.h"

#include "game/sim/bike_parts.h"
#include "game/sim/ground.h"

namespace rr::sim {
namespace {

constexpr uint32_t kGameState = 0x8005B2F8;
constexpr uint32_t kPools = 0x800CE4D0;      // 16 bytes a pool: base, stride, -> live, -> last index
constexpr uint32_t kViews = 0x800CD898;      // 1132 bytes a view record
constexpr uint32_t kP1Bike = 0x8005B38C, kP2Bike = 0x8005B21C;
constexpr uint32_t kExtraOn = 0x8005B314, kExtra = 0x8005B304;

uint32_t Players(GuestRam& g) { return g.U32(g.U32(kGameState) + 48u); }

// The rider of `bike` (or a passenger's rider): the bike's two view distances while it rides (+0x25C
// below 3), its own ViewDistance off it; then LodChoice. `bikeFor` is the bike whose distances it copies.
bool RiderLod(GuestRam& g, uint32_t rider, uint32_t bikeFor, uint32_t ownerBike) {
    if (g.U32(rider + 604u) < 3u) {
        g.W32(rider + 44u, g.U32(bikeFor + 44u));
        g.W32(g.U32(ownerBike) + 48u, g.U32(bikeFor + 48u)); // re-read through the owner (0x8008D1DC)
    } else {
        ViewDistance(g, rider);
    }
    return LodChoice(g, rider, static_cast<int32_t>(Players(g)));
}

// Pools 2 .. 5: ViewDistance on every entity with a handle and a live word, LodChoice for 2 and 3.
bool PoolLoop(GuestRam& g, uint32_t pool, bool lod) {
    const uint32_t rec = kPools + 16u * pool;
    int32_t n = g.S32(g.U32(rec + 12u));
    uint32_t e = g.U32(rec);
    for (; n >= 0; --n) {
        if (g.U16(e + 172u) != 0 && g.S16(e + 320u) != 0) {
            ViewDistance(g, e);
            if (lod && !LodChoice(g, e, static_cast<int32_t>(Players(g)))) return false;
        }
        e += g.U32(rec + 4u);
        if (g.Faulted()) return false;
    }
    return true;
}

} // namespace

bool ViewPass(GuestRam& g) {
    // 0x8008CFFC..: the counters saved, cleared; sp+16 = c0 - c1, sp+18 = c1 (u16)
    const uint16_t c0 = g.U16(kViewCounters), c1 = g.U16(kViewCounters + 2u);
    const int32_t thr[2] = {0x300, 0x1000};
    g.W16(kViewCounters + 2u, 0);
    g.W16(kViewCounters, 0);
    const uint16_t saved[2] = {static_cast<uint16_t>(c0 - c1), c1};
    {
        int32_t n = g.S32(g.U32(kPools + 12u));
        uint32_t e = g.U32(kPools);
        for (; n >= 0; --n) {                                                  // 0x8008D054
            if (g.S16(e + 320u) != 0) {
                ViewDistance(g, e);
                if (!LodChoice(g, e, static_cast<int32_t>(Players(g)))) return false;
                for (uint32_t p = 0; p < Players(g); ++p) {                    // 0x8008D0B0
                    const uint32_t v = kViews + 1132u * p;
                    const uint32_t ctr = kViewCounters + 2u * p;
                    if (g.U32(v + 548u) & 1u) {
                        g.W16(ctr, saved[p & 1u]);
                        continue;
                    }
                    const uint32_t bit = 16u << p;
                    const uint32_t f = g.U16(e + 320u) & ~bit;
                    const bool nearProg = static_cast<int32_t>(g.U32(e + 324u) - g.U32(v + 324u)) < 8192;
                    g.W16(e + 320u, static_cast<uint16_t>(f));
                    int32_t d = g.S32(e + 44u + 4u * p);
                    if (!(d < thr[nearProg ? 1 : 0])) continue;
                    if (!nearProg) d = static_cast<int32_t>(static_cast<uint32_t>(d) + static_cast<uint32_t>(thr[1]));
                    const int32_t q = d >> 6;
                    if (f & 0x10u) {
                        const uint32_t cur = g.U8(e + 850u);
                        g.W8(e + 850u, static_cast<uint8_t>(static_cast<int32_t>(cur) < q ? cur : static_cast<uint32_t>(q)));
                    } else {
                        g.W8(e + 850u, static_cast<uint8_t>(q));
                    }
                    g.W16(ctr, static_cast<uint16_t>(g.U16(ctr) + 1u));
                    g.W16(e + 320u, static_cast<uint16_t>(g.U16(e + 320u) | bit));
                }
            }
            const uint32_t rider = g.U32(e + 852u);                              // 0x8008D1A4
            if (g.S16(rider + 320u) != 0)
                if (!RiderLod(g, rider, e, e + 852u)) return false;
            e += g.U32(kPools + 4u);
            if (g.Faulted()) return false;
        }
    }
    g.W16(kViewCounters, static_cast<uint16_t>(g.U16(kViewCounters) + g.U16(kViewCounters + 2u))); // 0x8008D21C
    for (const uint32_t pb : {kP1Bike, kP2Bike}) {                              // the passengers
        const uint32_t b = g.U32(pb);
        if (pb == kP2Bike && b == 0) break;
        if (!(g.U8(g.U32(b + 852u) + 572u) & 0x10u)) {
            if (pb == kP1Bike) continue;
            break;
        }
        const uint32_t partner = g.U32(g.U32(b + 856u) + 852u);
        if (!RiderLod(g, partner, b, g.U32(b + 856u) + 852u)) return false;
    }
    // 0x8008D36C..: the records 0x800CE4F0 / 0x800CE500 (pools 2 and 3, the pedestrians and the cars: LodChoice too), then
    // 0x800CE510 / 0x800CE520 (pools 4 and 5, the props: the distance only) - gteproj2: this read 3 / 4 / 5 / 6,
    // which left the pedestrians' distances and LODs to whatever spawned them
    if (!PoolLoop(g, 2, true) || !PoolLoop(g, 3, true) || !PoolLoop(g, 4, false) || !PoolLoop(g, 5, false)) return false;
    if (g.U32(kExtraOn) != 0)                                                   // 0x8008D500
        for (uint32_t i = 0; i < 3; ++i) {
            const uint32_t e = g.U32(kExtra) + 280u * i;
            if (g.U16(e + 172u) != 0) ViewDistance(g, e);
        }
    return !g.Faulted();
}

} // namespace rr::sim
