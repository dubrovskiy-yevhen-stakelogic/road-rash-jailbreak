// The game modes in the product (race_modes.h): the race loader's per-race-type setup, transcribed from
// our own disassembly of RASHCDI.BIN (SHA-1 9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06), and the session's
// side of the PORTED mode functions of src\game\sim\modes.h (bench rows tools\rrverify\rows_modes.inc).
#include "game/race_modes.h"

#include <cstdio>
#include <cstring>

#include "game/race_session.h"
#include "game/loader_product.h"   // SetUpRace PORTED
#include "game/sim/grid_build.h"   // StartRecord RASHCDI 0x8006ACD0
#include "game/sim/ai.h"
#include "game/sim/coll_util.h"
#include "game/sim/fixed.h"
#include "game/sim/integrator.h"   // MulMatrix0 SLUS 0x8003FA40
#include "game/sim/modes.h"
#include "game/sim/race.h"         // ComputePlace SLUS 0x800138E8
#include "game/sim/recover_walk.h" // AxisRotation SLUS 0x8003FB34
#include "game/sim/road_runtime.h" // RoadTrack SLUS 0x8003701C
#include "game/sim/spine.h"        // ViewEvent 0x8008A998
#include "game/sim/stance.h"
#include "game/sim/traffic_bind.h" // CopJoin SLUS 0x80028034

namespace rr::game {

namespace {

using rr::sim::GuestRam;
using rr::sim::cu::U;

constexpr uint32_t kGp = 0x8005AC8C;          // SLUS_010.53's gp (race_session.cpp kArenaGp)
constexpr uint32_t kGsPtr = 0x8005B2F8;
constexpr uint32_t kRandSeed = kGp + 2076u;   // 0x8005B4A8, the shared LCG
constexpr uint32_t kStartDir = 0x800CF57C;    // the start-position record 0x800CF578's direction word
constexpr uint32_t kWanted = 0x800D8708;      // the police's wanted mask
constexpr uint32_t kRouteHeader = 0x800D6170; // +0x14 -> the start record {road, dir, along}
constexpr uint32_t kViewArray = 0x800CD898, kViewStride = 1132;
constexpr uint32_t kAltKind = 0x800541D4, kFightPtr = 0x8005AD4C;
constexpr uint32_t kModeSeamSp = 0x801FE400;  // ours: the recover seams' stack (race_session.cpp kRecoverSeamSp)
constexpr uint32_t kStepSp = 0x801FF000;      // ours: the world pass's stack (race_session.cpp kArenaStepSp)

uint8_t* Raw(uint8_t* ram, uint32_t a, uint32_t n) {
    if (a < 0x80000000u || static_cast<uint64_t>(a) + n > 0x80200000ull) return nullptr;
    return ram + (a - 0x80000000u);
}

// RASHCDI 0x800656A0, frame 56: bike `e`'s matrix +0x1B0 from slice `s` (the tangent row, the negated
// lateral and normal rows; the tangent and normal flipped for a negative +0x158), then turned by
// FixMul(+-10.0, 11) about the slice's lateral axis (AxisRotation SLUS 0x8003FB34, MulMatrix0 0x8003FA40).
void FaceTowardRoad(GuestRam& g, uint32_t e, uint32_t s, int32_t dir, uint32_t sp, const rr::sim::BikeTables& t) {
    const uint32_t f = sp - 56u;
    g.W16(e + 444u, static_cast<uint16_t>(0u - g.U16(s + 2u)));
    g.W16(e + 446u, static_cast<uint16_t>(0u - g.U16(s + 4u)));
    g.W16(e + 448u, static_cast<uint16_t>(0u - g.U16(s + 6u)));
    g.W16(e + 432u, g.U16(s + 14u));
    g.W16(e + 434u, g.U16(s + 16u));
    g.W16(e + 436u, g.U16(s + 18u));
    g.W16(e + 438u, static_cast<uint16_t>(0u - g.U16(s + 8u)));
    g.W16(e + 440u, static_cast<uint16_t>(0u - g.U16(s + 10u)));
    g.W16(e + 442u, static_cast<uint16_t>(0u - g.U16(s + 12u)));
    if (g.S32(e + 344u) < 0) {                                          // 0x80065764
        for (uint32_t o : {444u, 446u, 448u, 432u, 434u, 436u}) g.W16(e + o, static_cast<uint16_t>(0u - g.U16(e + o)));
    }
    const int32_t lat = g.S32(e + 344u);
    const bool plus = dir >= 0 ? !(lat > 0) : !(lat < 0);               // 0x800657AC..0x800657DC
    const int32_t ang = rr::sim::FixMul(plus ? 0xA0000 : static_cast<int32_t>(0xFFF60000u), 11);
    rr::sim::AxisRotation(g, s + 8u, ang, f + 16u, f, t);              // 0x800657F0
    int16_t a[9], b[9], o[9];
    for (uint32_t k = 0; k < 9; ++k) {
        a[k] = g.S16(e + 432u + 2u * k);
        b[k] = g.S16(f + 16u + 2u * k);
    }
    rr::sim::MulMatrix0(a, b, o);                                       // 0x80065800
    for (uint32_t k = 0; k < 9; ++k) g.W16(e + 432u + 2u * k, static_cast<uint16_t>(o[k]));
}

} // namespace

std::string ModeGrid(const uint8_t* gs, std::vector<StartEntry>& grid) {
    const uint32_t type = gs[4];
    if (!(type & 4u)) return {};
    size_t n = std::min<size_t>(grid.size(), 18u);                      // 0x80067C40: at most 18
    const uint8_t opt = gs[0x38];
    if (opt == 0) n = (type & 8u) ? 2u : 1u;                             // 0x80067C74
    else if (opt == 2) n = (type & 8u) ? 4u : 3u;
    char b[200];
    if (grid.size() < n) {
        std::snprintf(b, sizeof(b), "BuildGrid's race-type-%u grid wants %zu entries, the start block holds %zu", type, n,
                      grid.size());
        return b;
    }
    grid.resize(n);
    if (opt == 2 && n >= 3) {                                           // 0x80067EBC: two police bikes
        grid[1].slot = 17;
        grid[2].slot = 18;
    }
    std::snprintf(b, sizeof(b),
                  "BuildGrid RASHCDI 0x80067C1C / 0x80067EB4 (race type 0x%02X, option gs+0x38 = %u): %zu grid "
                  "entr%s%s", type, opt, n, n == 1 ? "y (the player alone)" : "ies",
                  opt == 2 ? ", entries 1 and 2 police (slots 17, 18)" : "");
    return b;
}

std::string ModeSetUpRace(GuestRam& g) {
    const uint32_t gs = g.U32(kGsPtr);
    g.W32(rr::sim::kModeMissionBase, 0);                                // 0x80063678..0x8006369C
    g.W32(0x8005ACCC, 0);
    g.W32(rr::sim::kModeTimeLimit, 0);
    g.W32(gs + 16u, 0);
    const uint32_t type = g.U8(gs + 4u);
    const uint32_t bank = g.U32(gs + 60u);
    // The commit RASHCDF 0x8007F37C's Time Trial arm (mode word 4, shell_logic.cpp CommitSelection): the
    // traffic switch 0x8005ACC4 = session+0x0A and the police switch 0x8005ACC0 = session+0x0B, the race
    // options; the session's own switches stay off where the arena has no traffic (race_session.cpp).
    if (g.U32(0x800D80D8u) == 4u) {
        if (g.S8(0x800D80E2u) == 0) g.W32(rr::sim::kModeTrafficOn, 0);
        if (g.S8(0x800D80E3u) == 0) g.W32(rr::sim::kModePoliceOn, 0);
    }
    if (type & 1u) {                                                    // 0x800636A4
        const uint32_t tab = (type & 0x10u) ? 0x80053090u : 0x80053084u;
        g.W32(rr::sim::kModeTimeLimit, U(g.S32(tab + (bank << 2)) * 300));
    }
    if (type == 36u) g.W32(rr::sim::kModeTimeLimit, g.U16(gs + 8u) * 300u);     // 0x80063718
    if (type == 33u) g.W32(rr::sim::kModeTimeLimit, U(g.S32(0x8005ADE0u) * 300)); // 0x8006374C
    if (type == 44u) {                                                  // 0x8006377C
        g.W32(rr::sim::kModeTimeLimit, U(g.S32(0x8005ADE4u) * 300));
        // BuildRace 0x80069858: the milestone phase, gs+0x0A, the police off, the route's direction
        g.W8(gs + 57u, 0);
        g.W8(gs + 10u, 0);
        g.W32(rr::sim::kModePoliceOn, 0);
        g.W32(rr::sim::kModeJbDir, g.U16(rr::sim::kModeMilestones + 2u) < g.U16(rr::sim::kModeMilestones) ? 0xFFFFFFFFu : 1u);
    }
    // 0x800637D8: 0x8006ACD0(start record) - the record 0x800CF578 {road, dir, along}
    const uint32_t start = g.U32(kRouteHeader + 0x14u);
    if (start != 0u) { // 0x8006ACD0(a, b, c): +0 = a & 0xFFFF, +4 = c (the direction), +8 = b (the along)
        g.W32(0x800CF578u, g.U32(start) & 0xFFFFu);
        g.W32(0x800CF57Cu, g.U32(start + 8u));
        g.W32(0x800CF580u, g.U32(start + 4u));
    }
    char b[240];
    std::snprintf(b, sizeof(b),
                  "SetUpRace RASHCDI 0x80063670 per race type (0x%02X): time limit %d ticks (%d s)%s; the start "
                  "record 0x800CF578 = *(0x800D6184) (0x8006ACD0's two road lookups not run)",
                  type, g.S32(rr::sim::kModeTimeLimit), g.S32(rr::sim::kModeTimeLimit) / 300,
                  type == 44u ? "; BuildRace's Jailbreak phase 0, police off" : "");
    return b;
}

uint32_t PlayerCopPlace(GuestRam& g, uint32_t e, const rr::sim::BikeTables& t) {
    const uint32_t sp = kStepSp, f = sp - 40u;                          // RASHCDI 0x8006581C
    uint32_t seed = g.U32(kRandSeed);
    const uint32_t v = rr::sim::Rand(seed);                             // SLUS 0x8001FC58
    g.W32(kRandSeed, seed);
    const uint32_t keep = ((v & 0x1000u) >> 12) | ((v >> 21) & 1u);
    const uint32_t s = g.U32(e + 340u);
    int32_t lat;
    if (keep) {
        lat = g.S32(e + 344u);
        g.W32(rr::sim::kModeRoadside, 0);
    } else {                                                            // 0x80065870: the piece's edge
        const uint32_t piece = g.U32(e + 372u);
        lat = g.S32(piece + 4u) >> 1;
        lat = g.S32(kStartDir) < 0 ? rr::sim::cu::Sub(g.S32(piece + 80u), lat) : rr::sim::cu::Add(lat, g.S32(piece + 208u));
        g.W32(rr::sim::kModeRoadside, 1);
    }
    rr::sim::cu::GMulAdd(g, s + 20u, s + 14u, g.S32(e + 348u), e + 184u);   // SLUS 0x8002EAD8
    rr::sim::cu::GMulAdd(g, e + 184u, s + 2u, lat, e + 184u);
    g.W32(e + 344u, U(lat));
    g.W32(e + 504u, g.U32(e + 184u));
    g.W32(e + 508u, g.U32(e + 188u));
    g.W32(e + 512u, g.U32(e + 192u));
    uint32_t v0 = 0;
    if (g.U32(rr::sim::kModeRoadside) == 0u) {
        FaceTowardRoad(g, e, s, g.S32(kStartDir), f, t);               // 0x80065914
        v0 = 1;
    }
    const uint32_t gs = g.U32(kGsPtr);
    g.W32(rr::sim::kModeQuota, g.U8(gs + 7u));                          // 0x80065938
    g.W32(kWanted, g.U32(kWanted) | (1u << (g.U8(gs + 6u) & 31u)));     // 0x80065950
    return v0;
}

std::string ModeSpawnBike(GuestRam& g, uint32_t e, bool player, const rr::sim::BikeTables& t) {
    const uint32_t gs = g.U32(kGsPtr);
    const uint32_t type = g.U8(gs + 4u);
    if (!(type & 1u)) return {};
    if (player) g.W32(e + 560u, g.U32(e + 560u) | 0x08000000u);         // 0x80066248: under AI control
    uint32_t start = 0;                                                  // sp+200 (0x80066874)
    const uint32_t rd = g.U32(e + 1084u);
    std::string line;
    if (g.U16(e + 172u) < g.U32(gs + 48u) && (g.U8(rd + 1u) & 0xFu) == 2u) {   // 0x80066878..0x800668A4
        g.W32(rr::sim::kModeArrestWord, g.U32(rr::sim::kModeArrestWord) | 1u);
        start = PlayerCopPlace(g, e, t);                                 // 0x800668DC (the road lookups
        // 0x8003662C / 0x8003DE28 before it are the session's SeatCursors / RoadTrack)
        char b[200];
        std::snprintf(b, sizeof(b),
                      "the player cop (RASHCDI 0x80066878, PlayerCopPlace 0x8006581C): arrest word 1, quota %u, "
                      "designated suspect handle %u, %s",
                      g.U32(rr::sim::kModeQuota), g.U8(gs + 6u),
                      start ? "in the line, first command 1 (waits behind the field)" : "at the roadside, command 4");
        line = b;
    }
    g.W16(e + 956u, start != 0u ? 1u : 4u);                              // 0x80066C10: {op, 224} at depth 1
    g.W16(e + 958u, 224);
    g.W8(e + 946u, 1);
    return line;
}

// ============================================================================ the session's side

namespace {

// The PORTED mode functions' callees in the product (modes.h ModeCallees).
class ProductModes final : public rr::sim::ModeCallees {
public:
    ProductModes(RaceSession& s, GuestRam& g, uint8_t* ram, rr::sim::StanceSeams* seams,
                 const std::function<bool(uint32_t, int32_t, int32_t&)>& place)
        : s_(s), g_(g), ram_(ram), seams_(seams), place_(place) {}

    bool SpeechCue(int32_t kind, uint32_t) override {                  // SLUS 0x8001B244, PORTED
        if (!s_.Sounds().SpeechCue(kind))
            s_.NoteSeam("SLUS 0x8001B244 SpeechCue (PORTED) was not run: no sound world is attached");
        return true;
    }
    bool ComputePlace(uint32_t e, int32_t mode, uint32_t, int32_t& v0) override { return place_(e, mode, v0); }
    bool CopJoin(uint32_t e, uint32_t) override {                       // SLUS 0x80028034, PORTED
        rr::sim::CopJoin(g_, e);
        return !g_.Faulted();
    }
    bool ViewEvent(uint32_t view, uint32_t mode, uint32_t) override {  // 0x8008A998, PORTED (spine.h)
        rr::sim::ViewEvent(g_, view, mode);
        return !g_.Faulted();
    }
    bool StanceEvent(uint32_t ev, uint32_t r, uint32_t p, uint32_t) override {   // 0x800C4550, PORTED
        if (seams_ == nullptr) return false;
        rr::sim::StanceLayer layer(g_, *seams_);
        layer.Event(ev, r, p);
        return !layer.Failed() && !g_.Faulted();
    }
    bool ClearCommands(uint32_t e, uint32_t) override {                 // 0x800BCD10, PORTED
        uint8_t* ep = Raw(ram_, e, 1096);
        if (ep == nullptr) return false;
        rr::sim::AiClearCommands(ep);
        return true;
    }
    bool PushCommand(uint32_t cmd, int32_t mode, uint32_t e, uint32_t) override {   // 0x800BCA68, PORTED
        uint8_t* ep = Raw(ram_, e, 1096);
        uint8_t* rec = Raw(ram_, cmd, 8);
        const uint32_t R = g_.U32(e + 852u);
        const uint32_t fp = g_.U32(kFightPtr);
        static const std::vector<uint8_t> noFight(12u * 256u, 0);
        if (ep == nullptr || rec == nullptr) return false;
        struct Sink final : rr::sim::AiStanceSink {
            ProductModes& m;
            bool ok = true;
            explicit Sink(ProductModes& x) : m(x) {}
            void PlayIdleStance(uint16_t event, uint32_t rider) override {
                if (!m.StanceEvent(event, rider, 2, 0)) ok = false;
            }
        } sink(*this);
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
    bool Remount(uint32_t e, int32_t fromRoad, uint32_t sp) override { // 0x800903F4, PORTED (crash.h)
        auto* rs = dynamic_cast<rr::sim::RiderSeams*>(seams_);
        if (rs == nullptr) return false;
        return s_.RecoverCall(0x800903F4u, {e, U(fromRoad)}, sp, *rs);
    }
    // ---- NOT ported, named
    bool SoundHold(int32_t, uint32_t) override {
        s_.NoteSeam("SLUS 0x80020E30 (the ambient voices' pitch hold, the player cop's chase start) is not ported: no effect");
        return true;
    }
    bool ArrestScene(uint32_t, uint32_t, uint32_t, uint32_t& v0) override {
        v0 = 0;
        s_.NoteSeam("RASHCDG 0x8009D664 (the arrest's police car and its camera shot) is not ported: answered 0, the "
                    "original's own no-car path");
        return true;
    }
    bool EscapeScene(int32_t block, uint32_t sp) override { return s_.ModeEscapeScene(block, sp); } // PORTED (jail_session.cpp)
    bool RiderModel(uint32_t r, uint32_t key, uint32_t) override {   // SLUS 0x800302C4 ModelKeySet, PORTED (traffic_bind.h)
        const int32_t v0 = rr::sim::ModelKeySet(g_, r, key);
        char b[360];
        std::snprintf(b, sizeof(b), "SLUS 0x800302C4 ModelKeySet (PORTED, traffic_bind.h) re-keyed a rider in Jailbreak "
                      "phase 3 (0x800C914C): key %u -> +0x4A %d, v0 %d%s", key, g_.S16(r + 0x4Au), v0, v0 < 0 ? " (no id in the page table 0x800D5F70: the product does not fill it, its renderer binds the sheets)" : "");
        s_.NoteSeam(b);
        return !g_.Faulted();
    }

private:
    RaceSession& s_;
    GuestRam& g_;
    uint8_t* ram_;
    rr::sim::StanceSeams* seams_;
    const std::function<bool(uint32_t, int32_t, int32_t&)>& place_;
};

} // namespace

bool RaceSession::ModePlace(uint32_t e, int32_t mode, int32_t& v0) {
    uint8_t* const ram = arena_.Ram();
    GuestRam g(ram, kGp);
    const int16_t armed = g.S16(rr::sim::kRouteRecordCount);
    std::vector<rr::sim::PlaceNode> pool(bikes_.size());
    for (size_t i = 0; i < bikes_.size(); ++i) {
        pool[i].entity = bikes_[i].entity.data();
        pool[i].riderDef = bikes_[i].riderDef.data();
        rr::sim::RouteBinding& b = pool[i].binding;
        b.routeObject = Raw(ram, g.U32(bikes_[i].entityAddress + 0x1ACu), 120);
        if (b.routeObject != nullptr) {
            b.firstWord = g.U32(g.U32(bikes_[i].entityAddress + 0x1ACu));
            b.legs = b.routeObject + 20;
            b.legCount = g.S32(g.U32(bikes_[i].entityAddress + 0x1ACu) + 0x0Cu);
        }
        b.routeArmed = armed;
    }
    rr::sim::ComputePlaceEnv env;
    env.gameState = gameState_.data();
    env.liveBikes = *liveBikes_;
    env.raceFlags = g.U32(rr::sim::kModeArrestWord);
    env.pool = pool.data();
    env.poolHigh = static_cast<int32_t>(bikes_.size()) - 1;
    env.poolCount = static_cast<int32_t>(bikes_.size());
    for (size_t i = 0; i < bikes_.size(); ++i)
        if (bikes_[i].entityAddress == e)
            return rr::sim::ComputePlace(bikes_[i].entity.data(), bikes_[i].riderDef.data(), pool[i].binding, mode, env, &v0);
    return false;
}

namespace {
template <class F>
bool RunMode(RaceSession& s, uint8_t* ram, rr::sim::StanceSeams* seams, const char* what, F&& f) {
    GuestRam g(ram, kGp);
    std::function<bool(uint32_t, int32_t, int32_t&)> place = [&](uint32_t e, int32_t mode, int32_t& v0) {
        return s.ModePlace(e, mode, v0);
    };
    ProductModes c(s, g, ram, seams, place);
    const bool ok = f(g, c) && !g.Faulted();
    if (!ok) s.NoteSeam(std::string(what) + " (PORTED, modes.h) refused: a callee failed or an address faulted");
    return ok;
}
} // namespace

bool RaceSession::ModeArrest(uint32_t cop, uint32_t t, uint32_t how, uint32_t sp) {
    ++modeCounts_.arrests;
    return RunMode(*this, arena_.Ram(), modeSeams_, "RASHCDG 0x80096F30 Arrest", [&](GuestRam& g, ProductModes& c) {
        return rr::sim::ModeArrest(g, cop, t, how, sp != 0u ? sp : kModeSeamSp, c);
    });
}

bool RaceSession::ModeArrestFsm(uint32_t e, int32_t dt, uint32_t sp, uint32_t& v0) {
    ++modeCounts_.fsm;
    return RunMode(*this, arena_.Ram(), modeSeams_, "RASHCDG 0x80096818 ArrestFsm", [&](GuestRam& g, ProductModes& c) {
        return rr::sim::ModeArrestFsm(g, e, dt, sp, c, v0);
    });
}

bool RaceSession::ModeJailRelease(uint32_t e, uint32_t sp) {
    ++modeCounts_.releases;
    return RunMode(*this, arena_.Ram(), modeSeams_, "RASHCDG 0x800A0708 JailRelease", [&](GuestRam& g, ProductModes& c) {
        return rr::sim::ModeJailRelease(g, e, sp, c);
    });
}

int32_t RaceSession::ModeMilestone() {
    ++modeCounts_.milestones;
    uint32_t v0 = 0;
    RunMode(*this, arena_.Ram(), modeSeams_, "RASHCDG 0x800C8D4C MilestoneAdvance", [&](GuestRam& g, ProductModes& c) {
        return rr::sim::ModeMilestoneAdvance(g, kModeSeamSp, c, v0);
    });
    return static_cast<int32_t>(v0);
}

void RaceSession::ModeMilestoneFirst() {
    GuestRam g(arena_.Ram(), kGp);
    if (!rr::sim::ModeMilestoneFirst(g, kModeSeamSp) || g.Faulted())
        NoteSeam("RASHCDG 0x800C92F8 MilestoneFirst (PORTED, modes.h) met an address the console would fault on");
}

bool RaceSession::ModeViewReset(uint32_t e) {
    GuestRam g(arena_.Ram(), kGp);
    const uint32_t view = kViewArray + kViewStride * g.U16(e + 172u);
    if (g.U32(view + 540u) == g.U32(view + 544u)) return !g.Faulted();  // 0x80095670
    rr::sim::ViewEvent(g, view, g.U32(view + 544u));                     // 0x8008A998(view, +0x220)
    if (g.Faulted()) return false;
    if (modeSeams_ == nullptr) return false;
    auto* rs = dynamic_cast<rr::sim::RiderSeams*>(modeSeams_);
    if (rs == nullptr) return false;
    return RecoverCall(0x80086AF8u, {view}, kModeSeamSp, *rs);         // CameraSpringReset, PORTED
}

void RaceSession::ModeSetUp() {
    GuestRam g(arena_.Ram(), kGp);
    NoteSeam(ModeSetUpRace(g));
    if (LoaderPorted()) { // SetUpRace RASHCDI 0x80063670 PORTED over the transcription above (which
        // keeps only the commit's Time Trial switches and BuildRace's Jailbreak arm of its own); its children run at
        // the session's own points - the start record PORTED here, the fight table and HazardPick answered with the
        // session's, the animation objects' count handed to the block after ModeSetUp (race_session.cpp)
        g.SetScratchpad(scratchpad_.data());
        LoaderTotals().animCount = -1;
        NoteSeam(LoaderSetUpRace(g, arena_.Ram(), nullptr, kStepSp,
                                 [&](uint32_t fn, const uint32_t* a, int, uint32_t sp, uint32_t& v0, bool& handled) {
                                     handled = true;
                                     switch (fn) {
                                     case 0x8006ACD0u: // StartRecord (PORTED, grid_build.h)
                                         v0 = static_cast<uint32_t>(rr::sim::StartRecord(g, a[0], a[1], a[2], sp));
                                         return !g.Faulted();
                                     case 0x800653E8u: // the fight table: LoadFightTable's host read
                                         v0 = g.U32(rr::sim::kLdFightPtr);
                                         return true;
                                     case 0x8006AD4Cu: // HazardPick (PORTED): the session's run (hazard_product.cpp)
                                         v0 = static_cast<uint32_t>(LoaderTotals().hazardPick);
                                         return true;
                                     case 0x8005D1A0u: // AnimObjectsInit (PORTED): after ModeSetUp, with this count
                                         LoaderTotals().animCount = static_cast<int32_t>(a[1]);
                                         return true;
                                     case rr::sim::kL2BuildRaceFn: // BuildRace PORTED, run at the grid's point
                                         if (!Loader2On()) break;
                                         Loader2Answered(Loader2Totals().buildRaceRan
                                                             ? "BuildRace 0x8006982C ran PORTED at the grid's point (grid_session.cpp)"
                                                             : "BuildRace 0x8006982C NOT run (the session's own grid layout)");
                                         return true;
                                     case 0x800244E0u: // GrfLoad, RoadLoad, StreamSetUp, StreamStart PORTED,
                                     case 0x8006AC6Cu: // run at the stream set-up / start (route_product.h)
                                     case 0x80022F78u:
                                     case 0x80023020u:
                                         if (!routePorted_) break;
                                         Loader2Answered("GrfLoad 0x800244E0 / RoadLoad 0x8006AC6C / StreamSetUp 0x80022F78 / "
                                                         "StreamStart 0x80023020 ran PORTED at the stream set-up and start (route_product.h)");
                                         return true;
                                     case rr::sim::kL2SirenSetFn: // SirenSet SLUS 0x80018DC8 PORTED
                                         if (!Loader2On()) break;
                                         rr::sim::SirenSet(g, static_cast<int32_t>(a[0]));
                                         ++Loader2Totals().sirenSet;
                                         Loader2Totals().siren = g.S32(g.gp() + 1908u);
                                         return !g.Faulted();
                                     case rr::sim::kL2FxGlobalsFn: // FxGlobalsInit SLUS 0x8002B83C PORTED
                                         if (!Loader2On()) break;
                                         rr::sim::FxGlobalsInit(g);
                                         ++Loader2Totals().fxGlobals;
                                         return !g.Faulted();
                                     case rr::sim::kLdRenderCamInitFn: { // RenderCamInit (PORTED): the render camera
                                         // pointer and record of view a0 (both views with two players); its GTE
                                         // SetGeomScreen / SetGeomOffset are the renderer's (DEFERRED)
                                         ProductLoaderCallees gte(g, nullptr, nullptr, nullptr, 0);
                                         ++LoaderTotals().renderCams;
                                         return rr::sim::RenderCamInit(g, a[0], sp, gte);
                                     }
                                     default:
                                         break;
                                     }
                                     handled = false;
                                     return true;
                                 }));
    }
    const rr::sim::BikeTables t = SessionTables();
    for (size_t i = 0; i < bikes_.size() && !gridPorted_; ++i) { // (the ported grid ran SpawnBike itself)
        const std::string line = ModeSpawnBike(g, bikes_[i].entityAddress, bikes_[i].isPlayer, t);
        if (!line.empty()) {
            NoteSeam(line);
            rr::sim::RoadTrack(g, bikes_[i].entityAddress, kStepSp);    // re-seat the moved bike's cursor
        }
    }
    if (g.Faulted()) {
        g.ClearFault();
        NoteSeam("the race loader's per-race-type setup (race_modes.cpp) met an address the console would fault on");
    }
}

std::string RaceSession::ModeTotals() const {
    char b[240];
    std::snprintf(b, sizeof(b),
                  "the game modes (modes.h, PORTED): Arrest %zu call(s), ArrestFsm %zu, JailRelease %zu, "
                  "MilestoneAdvance %zu; arrest word 0x%X, quota %d, phase gs+0x39 = %u, time limit %d ticks",
                  modeCounts_.arrests, modeCounts_.fsm, modeCounts_.releases, modeCounts_.milestones,
                  ArenaWord(rr::sim::kModeArrestWord), static_cast<int32_t>(ArenaWord(rr::sim::kModeQuota)),
                  gameState_[0x39], static_cast<int32_t>(ArenaWord(rr::sim::kModeTimeLimit)));
    std::string s = b;
    if (escapeScenes_ + jailStops_ + jailBoards_ != 0 || !passengers_.empty()) {   // jail_session.cpp
        char j[360];
        std::snprintf(j, sizeof(j), "; Jailbreak (sim\\jail.h, PORTED): EscapeScene %zu call(s), JailbreakFinish %zu frame(s), "
                      "JailBoard %zu", escapeScenes_, jailStops_, jailBoards_);
        s += j;
        for (const Passenger& ps : passengers_) {
            const uint32_t depth = ArenaHalf(ps.bike + 946u) & 0xFFu;
            std::snprintf(j, sizeof(j), "; passenger bike 0x%08X: command depth %u, top op %u, its rider +0x23C 0x%02X "
                          "stance %u mount %d", ps.bike, depth, depth != 0u ? ArenaHalf(ps.bike + 956u + 8u * (depth - 1u)) : 0u,
                          ArenaHalf(ps.rider + 572u) & 0xFFu, ArenaHalf(ps.rider + 544u),
                          static_cast<int8_t>(ArenaHalf(ps.rider + 72u) & 0xFFu));
            s += j;
        }
    }
    return s;
}

} // namespace rr::game
