#include "game/ai_race.h"

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <vector>

#include "game/sim/ai.h"
#include "game/sim/ai_cmd.h"
#include "game/sim/ai_brain.h"
#include "game/sim/ai_globals.h"
#include "game/sim/ai_plan.h"
#include "game/grid_loader.h"
#include "game/cop_race.h"
#include "game/cheats.h" // the cheat menu's passive rivals

namespace rr::game {

namespace {
constexpr uint32_t kGp = 0x8005AC8C; // SLUS_010.53's gp (race_session.cpp kArenaGp)
constexpr uint32_t kGameStatePtr = 0x8005B2F8, kRaceFlags = 0x8005AD48, kAltKind = 0x800541D4,
                   kFightPtr = 0x8005AD4C, kAttackers = 0x800CCAC0, kSqrtTable = 0x800560CC;

uint8_t* Raw(uint8_t* ram, uint32_t a, uint32_t n) {
    if (a < 0x80000000u || static_cast<uint64_t>(a) + n > 0x80200000ull) return nullptr;
    return ram + (a - 0x80000000u);
}

// The command pass's callees in the product (ai_race.h RunAiCommandPass).
struct ProductCmdCallees final : rr::sim::AiCmdCallees {
    uint8_t* ram;
    rr::sim::GuestRam& g;
    const AiProductHooks& hooks;
    AiPassCounts& counts;
    ProductCmdCallees(uint8_t* r, rr::sim::GuestRam& gg, const AiProductHooks& h, AiPassCounts& c)
        : ram(r), g(gg), hooks(h), counts(c) {}
    const int16_t* Sqrt() { return reinterpret_cast<const int16_t*>(Raw(ram, kSqrtTable, 2)); }

    bool CmdRace(uint32_t e, uint32_t) override {
        uint8_t* ep = Raw(ram, e, 1096);
        uint8_t* rd = Raw(ram, g.U32(e + 1084u), 72);
        const uint8_t* gs = Raw(ram, g.U32(kGameStatePtr), 64);
        const uint8_t* sl = Raw(ram, g.U32(e + 340u), 52);
        if (ep == nullptr || rd == nullptr || gs == nullptr || sl == nullptr) return false;
        rr::sim::AiCmdRaceEnv ce;
        ce.gameState = gs;
        ce.riderDef = rd;
        ce.slice = reinterpret_cast<const int16_t*>(sl);
        ce.flags = g.U32(kRaceFlags);
        ce.sqrtTable = Sqrt();
        rr::sim::AiCmdRace(rr::sim::EntityView(ep), ce);
        return true;
    }
    bool PopCommand(uint32_t e, uint32_t) override {
        uint8_t* ep = Raw(ram, e, 1096);
        const uint32_t R = g.U32(e + 852u);
        struct Sink final : rr::sim::AiStanceSink {
            const AiProductHooks& h;
            bool ok = true;
            explicit Sink(const AiProductHooks& x) : h(x) {}
            void PlayIdleStance(uint16_t event, uint32_t rider) override {
                if (!h.stanceEvent(event, rider, 2)) ok = false;
            }
        } sink(hooks);
        static const std::vector<uint8_t> noFight(12u * 256u, 0);
        rr::sim::AiPopEnv pe;
        pe.gameState = Raw(ram, g.U32(kGameStatePtr), 64);
        pe.riderDef = Raw(ram, g.U32(e + 1084u), 72);
        pe.rider = Raw(ram, R, 640);
        pe.riderAddress = R;
        pe.altKindTable = Raw(ram, kAltKind, 8u * 65536u);
        const uint32_t fp = g.U32(kFightPtr);
        pe.fightRecords = fp != 0u ? Raw(ram, fp, 12u * 256u) : noFight.data();
        pe.attackerMask = reinterpret_cast<const uint16_t*>(Raw(ram, kAttackers, 16));
        pe.stance = &sink;
        if (ep == nullptr || pe.gameState == nullptr || pe.riderDef == nullptr || pe.altKindTable == nullptr ||
            pe.fightRecords == nullptr)
            return false;
        rr::sim::AiPopCommand(ep, pe);
        ++counts.pops;
        return sink.ok;
    }
    bool SetAimDelta(uint32_t e, int32_t scalar, uint32_t) override {
        uint8_t* ep = Raw(ram, e, 1096);
        if (ep == nullptr) return false;
        rr::sim::SetAimDelta(rr::sim::EntityView(ep), nullptr, reinterpret_cast<const int16_t*>(ep + 0x368), &scalar,
                             1, Sqrt());
        ++counts.aims;
        return true;
    }
    bool StanceEvent(uint32_t ev, uint32_t r, uint32_t p, uint32_t) override { return hooks.stanceEvent(ev, r, p); }
    bool Unported(const char* what) {
        ++counts.unported;
        hooks.seam(std::string(what) + " is not ported: the AI command pass runs on without it (no effect, v0 = 0)");
        return true;
    }
    bool CopIdle(uint32_t e, uint32_t cmd, int32_t dt, uint32_t sp) override { // 0x800BAA2C, PORTED (cop_race.h)
        return ProductCopIdle(ram, g, hooks, counts, e, cmd, dt, sp);
    }
    bool CopRelease(uint32_t e, uint32_t sp) override {
        if (hooks.copRelease) return hooks.copRelease(e, sp); // ClimbDone, PORTED (recover_race.h)
        return Unported("RASHCDG 0x80092AD4 (op 1, a cop released)");
    }
    bool LeaveRace(uint32_t e, int32_t dt, uint32_t sp) override {
        if (hooks.leaveRace) return hooks.leaveRace(e, dt, sp); // RiderRecover, PORTED (recover_race.h)
        return Unported("RASHCDG 0x80092E04 (op 18, leave the race)");
    }
    bool Fight(uint32_t e, uint32_t target, int32_t, uint32_t sp) override {
        if (target < 64u && CheatRivalPassive(g, e, g.U32(0x8005B3A0u) + 1096u * target)) { // cheats.h: passive -
            CheatNotePassive(1);                // the fight ends as FightContinue 0x800C0BE8 ends one (AiPopCommand)
            return PopCommand(e, sp);
        }
        if (hooks.fight) { // PORTED (fight_session.cpp)
            const CheatLift lift = target < 64u ? CheatSparringLift(g, e, g.U32(0x8005B3A0u) + 1096u * target) : CheatLift{};
            // cheats.h, the CHEAT RULE: a sparring partner may attack the standing player
            const CheatSpeedLift stand = target < 64u ? CheatStandingLift(g, e, g.U32(0x8005B3A0u) + 1096u * target) : CheatSpeedLift{};
            const bool ok = hooks.fight(e, target);
            CheatStandingDrop(g, stand);
            CheatSparringDrop(g, lift); // cheats.h: a stopped sparring partner, riding for the player's blow only
            return ok;
        }
        return Unported("RASHCDG 0x800C035C FightUpdate (op 16)");
    }
    bool Intercept(uint32_t e, uint32_t t, uint32_t sp) override { // 0x800BBEBC, PORTED (cop_race.h)
        return ProductCopIntercept(ram, g, hooks, counts, e, t, sp);
    }
    bool Strike(uint32_t e, uint32_t t, uint32_t) override {
        if (CheatRivalPassive(g, e, t)) { // cheats.h: a passive rival's op 9 does not strike (op 8's pass follows)
            CheatNotePassive(2);
            return true;
        }
        if (hooks.strike) return hooks.strike(e, t); // PORTED (sim\strike.h, fight_session.cpp StrikeCommand)
        return Unported("RASHCDG 0x800C1370 (op 9's strike)");
    }
    bool JailbreakFinish(uint32_t sp) override {
        if (hooks.jailbreakFinish) return hooks.jailbreakFinish(sp); // PORTED (jail_session.cpp)
        return Unported("RASHCDG 0x800C9E74 (op 2, a Jailbreak finish)");
    }
    bool CopGap(uint32_t e, uint32_t t, uint32_t sp, int32_t& v0) override { // 0x8009E444, PORTED (cop_race.h)
        v0 = ProductCopGap(g, e, t, sp);
        return !g.Faulted();
    }
};
} // namespace

bool RunAiCommandPass(uint8_t* ram, uint32_t gp, int32_t dt, uint32_t skip, uint32_t maskB, uint32_t sp,
                      const AiProductHooks& hooks, AiPassCounts& counts) {
    rr::sim::GuestRam g(ram, gp);
    {   // what the pass will dispatch, for the frame log
        const uint32_t pool = g.U32(0x8005B3A0u);
        const int32_t n = g.S32(0x8005B1F8u);
        for (int32_t i = 0; i < n && i < 64; ++i) {
            if (skip & (1u << (static_cast<uint32_t>(i) & 31u))) continue;
            const uint32_t e = pool + 1096u * static_cast<uint32_t>(i);
            const uint32_t op = g.U16(e + 948u + 8u * static_cast<uint32_t>(static_cast<int32_t>(g.S8(e + 946u))));
            ++counts.byOp[op < 19u ? op : 19u];
        }
    }
    ProductCmdCallees c(ram, g, hooks, counts);
    const bool ok = rr::sim::AiCommandPass(g, dt, skip, maskB, sp, c, c.Sqrt());
    if (g.Faulted()) return false;
    return ok;
}

namespace {

// AiPushCommand (ai.h) on the arena: the 8-byte record at guest `cmd` read, pushed, written back.
bool ProductPush(uint8_t* ram, rr::sim::GuestRam& g, const AiProductHooks& hooks, AiPassCounts& counts,
                 uint32_t cmd, int32_t mode, uint32_t e, int32_t& v0) {
    uint8_t* ep = Raw(ram, e, 1096);
    uint8_t* rec = Raw(ram, cmd, 8);
    const uint32_t R = g.U32(e + 852u);
    static const std::vector<uint8_t> noFight(12u * 256u, 0);
    const uint32_t fp = g.U32(kFightPtr);
    const uint8_t* fight = fp != 0u ? Raw(ram, fp, 12u * 256u) : noFight.data();
    const uint8_t* alt = Raw(ram, kAltKind, 8u * 65536u);
    if (ep == nullptr || rec == nullptr || fight == nullptr || alt == nullptr) return false;
    struct Sink final : rr::sim::AiStanceSink {
        const AiProductHooks& h;
        bool ok = true;
        explicit Sink(const AiProductHooks& x) : h(x) {}
        void PlayIdleStance(uint16_t event, uint32_t rider) override {
            if (!h.stanceEvent(event, rider, 2)) ok = false;
        }
    } sink(hooks);
    rr::sim::AiPushEnv pe;
    pe.raceClock = g.S32(g.U32(kGameStatePtr) + 0x10u);
    pe.rider = Raw(ram, R, 640);
    pe.riderAddress = R;
    pe.altKindTable = alt;
    pe.fightRecords = fight;
    pe.stance = &sink;
    uint8_t c[8];
    std::memcpy(c, rec, 8);
    v0 = rr::sim::AiPushCommand(c, mode, ep, pe);
    std::memcpy(rec, c, 8);
    ++counts.pushes;
    return sink.ok;
}

struct ProductPlanCallees final : rr::sim::AiPlanCallees {
    uint8_t* ram;
    rr::sim::GuestRam& g;
    const AiProductHooks& hooks;
    AiPassCounts& counts;
    ProductPlanCallees(uint8_t* r, rr::sim::GuestRam& gg, const AiProductHooks& h, AiPassCounts& c)
        : ram(r), g(gg), hooks(h), counts(c) {}
    bool CanEngage(uint32_t e, uint32_t t, int32_t lateral, uint32_t, uint32_t& v0) override {
        if (CheatRivalPassive(g, e, t)) { // cheats.h: a passive rival never engages a player
            CheatNotePassive(0);
            v0 = 0;
            return true;
        }
        const CheatSpeedLift stand = CheatStandingLift(g, e, t); // cheats.h: the CHEAT RULE, standing player
        v0 = static_cast<uint32_t>(rr::sim::AiStrikeReach(g, e, t, lateral)); // 0x800BC1EC, PORTED (ai_cmd.h)
        CheatStandingDrop(g, stand);
        return !g.Faulted();
    }
    bool ChaseTest(uint32_t e, uint32_t t, int32_t lateral, uint32_t, uint32_t& v0) override {
        v0 = static_cast<uint32_t>(rr::sim::AiPassOk(g, e, t, lateral));     // 0x800BBD44, PORTED (ai_cmd.h)
        return !g.Faulted();
    }
    bool Unported(const char* what) {
        ++counts.unported;
        hooks.seam(std::string(what) + " is not ported: the AI planner runs on without it (no effect)");
        return true;
    }
    bool CopTail(uint32_t e, uint32_t sp) override { return ProductCopTail(ram, g, hooks, counts, e, sp); } // PORTED (cop_race.h)
    bool CopArrestScan(uint32_t e, uint32_t sp) override { // 0x80097388, PORTED (cop_race.h)
        return ProductCopArrestScan(ram, g, hooks, counts, e, sp);
    }
    bool RiderVoice(uint32_t h, int32_t crash, uint32_t sp) override {
        if (hooks.riderSpeech) return hooks.riderSpeech(h, crash, sp); // PORTED (speech_session.cpp)
        return Unported("SLUS 0x8001A760 (the rider's voice line before a fight)");
    }
    bool ComputePlace(uint32_t e, int32_t mode, uint32_t, int32_t& place) override {
        return hooks.computePlace && hooks.computePlace(e, mode, &place);
    }
    bool PushCommand(uint32_t cmd, int32_t mode, uint32_t e, uint32_t, int32_t& v0) override {
        return ProductPush(ram, g, hooks, counts, cmd, mode, e, v0);
    }
    bool StanceEvent(uint32_t ev, uint32_t r, uint32_t p, uint32_t) override { return hooks.stanceEvent(ev, r, p); }
};

struct ProductBrainCallees final : rr::sim::AiBrainCallees {
    uint8_t* ram;
    rr::sim::GuestRam& g;
    const AiProductHooks& hooks;
    AiPassCounts& counts;
    ProductCmdCallees cmd;
    ProductBrainCallees(uint8_t* r, rr::sim::GuestRam& gg, const AiProductHooks& h, AiPassCounts& c)
        : ram(r), g(gg), hooks(h), counts(c), cmd(r, gg, h, c) {}
    bool PushCommand(uint32_t c, int32_t mode, uint32_t e, uint32_t) override {
        int32_t v0 = 0;
        return ProductPush(ram, g, hooks, counts, c, mode, e, v0);
    }
    bool PopCommand(uint32_t e, uint32_t sp) override { return cmd.PopCommand(e, sp); }
    bool SetAimDelta(uint32_t e, uint32_t target, uint32_t dir, uint32_t scalar, int32_t mode, uint32_t) override {
        uint8_t* ep = Raw(ram, e, 1096);
        if (ep == nullptr) return false;
        const int32_t* t = target != 0u ? reinterpret_cast<const int32_t*>(Raw(ram, target, 12)) : nullptr;
        const int16_t* d = dir != 0u ? reinterpret_cast<const int16_t*>(Raw(ram, dir, 6)) : nullptr;
        const int32_t* sc = scalar != 0u ? reinterpret_cast<const int32_t*>(Raw(ram, scalar, 4)) : nullptr;
        if ((target != 0u && t == nullptr) || (dir != 0u && d == nullptr) || (scalar != 0u && sc == nullptr))
            return false;
        rr::sim::SetAimDelta(rr::sim::EntityView(ep), t, d, sc, mode, cmd.Sqrt());
        ++counts.aims;
        return true;
    }
    bool GapTest(uint32_t e, uint32_t other, int32_t gap, uint32_t, uint32_t& v0) override {
        v0 = static_cast<uint32_t>(rr::sim::AiGapOk(g, e, other, gap));      // 0x800BB448, PORTED (ai_cmd.h)
        return !g.Faulted();
    }
    bool CloseTest(uint32_t e, uint32_t other, int32_t gap, uint32_t, uint32_t& v0) override {
        v0 = static_cast<uint32_t>(rr::sim::AiCloseOk(g, e, other, gap));    // 0x800BBBB8, PORTED (ai_cmd.h)
        return !g.Faulted();
    }
};

} // namespace

bool RunAiPlan(uint8_t* ram, uint32_t gp, int32_t acc, uint32_t sp, const AiProductHooks& hooks,
               AiPassCounts& counts) {
    rr::sim::GuestRam g(ram, gp);
    ProductPlanCallees c(ram, g, hooks, counts);
    const bool ok = rr::sim::AiPlan(g, acc, sp, c);
    return ok && !g.Faulted();
}

bool RunAiBrainPass(uint8_t* ram, uint32_t gp, int32_t dt, uint32_t skip, uint32_t maskC, uint32_t sp,
                    const AiProductHooks& hooks, AiPassCounts& counts) {
    rr::sim::GuestRam g(ram, gp);
    ProductBrainCallees c(ram, g, hooks, counts);
    const bool ok = rr::sim::AiBrainPass(g, dt, skip, maskC, sp, c);
    return ok && !g.Faulted();
}

size_t CheckAiRiderRecords(const uint8_t* ram, const std::vector<uint8_t>& bi, bool mutate, std::string& report);

bool CheckAiGlobalsArena(const DiscImage& disc, const std::string& path, std::string& report, bool mutate) {
    namespace fs = std::filesystem;
    std::vector<uint8_t> file;
    if (const auto f = disc.Find("DATA/GLOBALS.BI")) file = disc.ReadFile(*f);
    if (file.empty()) {
        report = "aiglobalscheck: DATA/GLOBALS.BI is not on the disc";
        return false;
    }
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
    size_t pass = 0, fail = 0;
    report.clear();
    for (const std::string& p : images) {
        std::ifstream in(p, std::ios::binary);
        std::vector<uint8_t> ram((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (ram.size() < rr::sim::GuestRam::kRamSize) continue;
        std::string line;
        size_t differ = rr::sim::CheckAiGlobals(ram.data(), kGp, file, mutate, line);
        {   // and the rider records' AI part against LEVEL<bank+1>.BI (ai_race.h LoadAiRiderRecords)
            rr::sim::GuestRam g(ram.data(), kGp);
            const int32_t bank = g.S32(g.U32(kGameStatePtr) + 0x3Cu);
            std::vector<uint8_t> bi;
            if (bank >= 0 && bank < 3)
                if (const auto f = disc.Find("DATA/LEVEL" + std::to_string(bank + 1) + ".BI")) bi = disc.ReadFile(*f);
            std::string rline = "no LEVEL.BI";
            differ += bi.size() >= 20u * 64u ? CheckAiRiderRecords(ram.data(), bi, mutate, rline) : 1u;
            line += "; " + rline;
        }
        (differ == 0 ? pass : fail) += 1;
        report += (differ == 0 ? "  same   " : "  DIFFER ") + p + ": " + line + "\n";
    }
    report += "aiglobalscheck" + std::string(mutate ? "-mutate" : "") + ": " + std::to_string(pass) + " of " +
              std::to_string(pass + fail) + " image(s) hold GLOBALS.BI exactly where RASHCDI 0x80064610 puts it, and LEVEL<n>.BI in their rider records";
    return fail == 0 && pass > 0;
}

} // namespace rr::game

namespace rr::game {

int AutoSteer::Decide(const uint8_t* ram, uint32_t entity, bool& throttle, bool& brake) {
    auto at = [&](uint32_t a) -> const uint8_t* { return ram + (a & 0x1FFFFFu); };
    auto w32 = [&](uint32_t a) {
        const uint8_t* p = at(a);
        return static_cast<int32_t>(static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                                    (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24));
    };
    auto s16 = [&](uint32_t a) {
        const uint8_t* p = at(a);
        return static_cast<int32_t>(static_cast<int16_t>(static_cast<uint16_t>(p[0] | (p[1] << 8))));
    };
    const uint32_t r = static_cast<uint32_t>(w32(entity + 0x168u));
    if (r != road) {
        road = r;
        routeDir = w32(entity + 0x16Cu) < 0 ? -1 : 1;
    }
    const int32_t lat = w32(entity + 0x158u);
    const int64_t d = have ? static_cast<int64_t>(lat) - prevLat : 0;
    prevLat = lat;
    have = true;
    // the heading against the road's sense (both unit vectors x 4096)
    const uint32_t slice = static_cast<uint32_t>(w32(entity + 0x154u));
    int64_t along = 4096LL * 4096LL, cross = 0;
    if (slice >= 0x80000000u && slice < 0x801FFFC0u) {
        const int64_t tx = routeDir * s16(slice + 14u), tz = routeDir * s16(slice + 18u);
        const int64_t hx = s16(entity + 0x1C2u), hz = s16(entity + 0x1C6u);
        along = hx * tx + hz * tz;
        cross = hz * tx - hx * tz;
    }
    if (path != nullptr && !path->empty()) {
        // pure pursuit of the route 30 units ahead (x/z plane)
        const std::vector<rr::RoadSlice>& P = *path;
        const double px = w32(entity + 0xB8u), pz = w32(entity + 0xC0u);
        auto d2 = [&](size_t k) {
            const double dx = P[k].pos[0] - px, dz = P[k].pos[2] - pz;
            return dx * dx + dz * dz;
        };
        const size_t lo = hint > 64 ? hint - 64 : 0, hi = std::min(P.size(), hint + 65);
        size_t best = hint < P.size() ? hint : 0;
        for (size_t k = lo; k < hi; ++k)
            if (d2(k) < d2(best)) best = k;
        hint = best;
        size_t tgt = best;
        while (tgt + 1 < P.size() && P[tgt].distance - P[best].distance < 30u * 65536u) ++tgt;
        double tx = P[tgt].pos[0], tz = P[tgt].pos[2];
        if (lane != 0.0 && tgt > best) {                  // the lane: beside the route, across its direction
            const double ex = static_cast<double>(P[tgt].pos[0]) - P[best].pos[0];
            const double ez = static_cast<double>(P[tgt].pos[2]) - P[best].pos[2];
            const double el = std::sqrt(ex * ex + ez * ez);
            if (el > 1.0) {
                tx += -ez / el * lane * 65536.0;
                tz += ex / el * lane * 65536.0;
            }
        }
        static const bool dodgeOff = [] {   // RRJB_DODGE=off: the pure pursuit alone (the negative control)
            const char* v = std::getenv("RRJB_DODGE");
            return v != nullptr && v[0] == 'o' && v[1] == 'f';
        }();
        {   // a pole in the corridor to the pursuit point: aim beside it, as a player steers round one
            const double ax = tx - px, az = tz - pz, al = std::sqrt(ax * ax + az * az);
            if (al > 65536.0 && !dodgeOff) {
                const double ux = ax / al, uz = az / al, nx = -uz, nz = ux;
                const double reach = std::min(al, 25.0 * 65536.0);
                const double half = std::max(0.0, static_cast<double>(w32(entity + 304u)));   // the box's half width
                double bestF = 1e30, bestL = 0, bestClear = 0;
                auto consider = [&](uint32_t sh, double r) {
                    const double dx = w32(sh + 12u) - px, dz = w32(sh + 20u) - pz;
                    const double f = dx * ux + dz * uz, l = dx * nx + dz * nz;
                    const double clear = r + half + 0.75 * 65536.0;
                    if (f < -0.5 * 65536.0 || f > reach || std::fabs(l) >= clear || f >= bestF) return;
                    bestF = f;
                    bestL = l;
                    bestClear = clear;
                };
                // the poles the collision pass meets (solid_product.cpp Poles): pool-6 volumes with a class,
                // pool-4 props whose model is a pole and that are not moving; radius +0x84 (a prop's smaller
                // of +0x84 / +0x88)
                const uint32_t v6 = static_cast<uint32_t>(w32(0x800CD6C4u));
                const int32_t hi6 = w32(0x800CD6A8u + 8u);
                for (int32_t i = 0; v6 >= 0x80000000u && i <= hi6 && i < 32; ++i) {
                    const uint32_t v = v6 + 280u * static_cast<uint32_t>(i);
                    if ((w32(v) & 0xFFFF) != 0 && w32(v + 8u) != 0) consider(v, w32(v + 132u));
                }
                const uint32_t b4 = static_cast<uint32_t>(w32(0x800CE510u));
                const uint32_t hi4p = static_cast<uint32_t>(w32(0x800CE510u + 12u));
                const int32_t hi4 = hi4p >= 0x80000000u ? w32(hi4p) : -1;
                for (int32_t i = 0; b4 >= 0x80000000u && i <= hi4 && i < 32; ++i) {
                    const uint32_t p = b4 + 596u * static_cast<uint32_t>(i);
                    if ((w32(p + 172u) & 0xFFFF) == 0) continue;
                    const uint32_t model = static_cast<uint32_t>(w32(p));
                    if (model < 0x80000000u || !(s16(model + 14u) & 2) || (w32(p + 592u) & 0x1A06)) continue;
                    consider(p + 172u, std::min(w32(p + 172u + 132u), w32(p + 172u + 136u)));
                }
                if (bestF < 1e29) {
                    const double side = bestL >= 0 ? bestL - bestClear : bestL + bestClear;
                    const double f = std::max(bestF, 2.0 * 65536.0);
                    tx = px + ux * f + nx * side;
                    tz = pz + uz * f + nz * side;
                    ++poleDodges;
                }
            }
        }
        const double vx = tx - px, vz = tz - pz;
        const double hx = s16(entity + 0x1C2u), hz = s16(entity + 0x1C6u);
        const double vl = std::sqrt(vx * vx + vz * vz), hl = std::sqrt(hx * hx + hz * hz);
        if (vl > 1.0 && hl > 1.0) {
            const double c = (hz * vx - hx * vz) / (vl * hl), dotv = (hx * vx + hz * vz) / (vl * hl);
            throttle = dotv > 0.94;                   // within 20 degrees of the pursuit point
            brake = dotv < 0.5 && w32(entity + 0x1E0u) > 0x100000;
            if (dotv > 0.998) return 0;               // within 3.6 degrees: hold straight
            return (dotv < 0.0 ? (c >= 0 ? 1 : -1) : (c > 0 ? 1 : -1)) * turnSense;
        }
    }
    if (along < 2048LL * 4096LL) {                    // more than 60 degrees off: turn back, off the throttle
        throttle = false;
        brake = false;
        return cross > 0 ? turnSense : -turnSense;
    }
    // offset plus twenty frames of its drift, in world units x 65536
    const int64_t u = routeDir * (static_cast<int64_t>(lat) + 20 * d);
    const int64_t mag = u < 0 ? -u : u;
    const int64_t drift = d < 0 ? -d : d;
    throttle = mag < 0x80000 && drift < 0x6000;       // under 8 units off, drifting under 3/8 unit a frame
    brake = mag > 0xC0000 && drift > 0x3000;          // over 12 units off and still drifting
    const int64_t dead = 0x8000; // half a unit
    if (u > dead) return sense;
    if (u < -dead) return -sense;
    return 0;
}

} // namespace rr::game

namespace rr::game {

namespace {
constexpr uint32_t kAiToHandle = 0x800D38C8, kHandleToAi = 0x800D38B0;
// LEVEL<n>.BI: runtime offset <- file offset, one byte each.
struct BiByte {
    uint8_t rt, file;
};
std::vector<BiByte> BiMapping() {
    std::vector<BiByte> m = {{0x01, 0x05}, {0x02, 0x06}, {0x03, 0x07}, {0x08, 0x08}, {0x09, 0x09}, {0x0A, 0x0A},
                             {0x0B, 0x0B}, {0x0C, 0x10}, {0x0D, 0x11}, {0x0E, 0x12}, {0x0F, 0x13}, {0x2C, 0x28},
                             {0x2D, 0x29}, {0x3D, 0x35}};
    for (uint8_t j = 0; j < 20; ++j) m.push_back({static_cast<uint8_t>(0x10 + j), static_cast<uint8_t>(0x14 + j)});
    for (uint8_t j = 0; j < 8; ++j) m.push_back({static_cast<uint8_t>(0x34 + j), static_cast<uint8_t>(0x2C + j)});
    return m;
}
} // namespace

size_t CheckAiRiderRecords(const uint8_t* ram, const std::vector<uint8_t>& bi, bool mutate, std::string& report) {
    auto u32 = [&](uint32_t a) {
        uint32_t v;
        std::memcpy(&v, ram + (a & 0x1FFFFFu), 4);
        return v;
    };
    const uint32_t pool = u32(0x8005B3A0u);
    const uint32_t n = u32(0x8005B1F8u);
    size_t checked = 0, differ = 0;
    const std::vector<BiByte> map = BiMapping();
    for (uint32_t i = 0; i < n && i < 18; ++i) {
        const uint32_t e = pool + 1096u * i;
        const uint32_t h = u32(e + 0xACu) & 0xFFFFu;
        if (h >= 18) continue;
        const uint32_t k = ram[(kHandleToAi + h) & 0x1FFFFFu];
        if (k >= 20 || ram[(kAiToHandle + k) & 0x1FFFFFu] != h) { ++differ; continue; }  // the maps are inverses
        const uint32_t rd = u32(e + 0x43Cu);
        // the static fields: the class nibble, the decay divisor, reach, strength, +0x0D, max health, the
        // swing counters, and the grudge bytes the race never writes (all but toward AI 0)
        for (const BiByte& b : map) {
            if (b.rt == 0x02 || b.rt == 0x0F || b.rt == 0x3D || b.rt == 0x10 || b.rt == 0x2C || b.rt == 0x2D)
                continue;   // runtime-written: mood, health, combo, the grudge toward AI 0, the weapon mask
            const uint32_t kr = mutate ? (k + 1u) % 20u : k;   // the control: the neighbouring record
            uint8_t v = ram[(rd + b.rt) & 0x1FFFFFu], f = bi[64u * kr + b.file];
            if (b.rt == 0x01) { v &= 0x0F; f &= 0x0F; }
            ++checked;
            if (v != f) ++differ;
        }
    }
    report = std::to_string(checked) + " rider-record byte(s), " + std::to_string(differ) + " differ";
    if (checked == 0) report = "no race bikes in this image";
    return differ;
}

} // namespace rr::game
