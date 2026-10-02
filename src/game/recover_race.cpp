#include "game/recover_race.h"
#include "game/rumble_product.h" // the pad rumble

#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <vector>

#include "game/sim/ai.h"
#include "game/sim/anim.h"
#include "game/sim/bike.h"
#include "game/sim/camera.h"
#include "game/sim/collision.h"
#include "game/sim/crash.h"
#include "game/sim/ground.h"
#include "game/sim/population.h"
#include "game/sim/present.h"
#include "game/sim/race.h"
#include "game/sim/recover_air.h"
#include "game/sim/recover_fall.h"
#include "game/sim/recover_ground.h"
#include "game/sim/recover_walk.h"
#include "game/sim/ped_hit.h" // PedHit 0x800A9868
#include "game/sim/peds.h" // the pedestrians of pool 2
#include "game/sim/partners.h" // PedVoice SLUS 0x8001B44C
#include "game/strike_product.h"
#include "game/sim/road_query.h"
#include "game/sim/road_runtime.h"
#include "game/sim/spine.h"
#include "game/sim/traffic.h"
#include "game/sim/traffic_bind.h"
#include "game/race_session.h"

namespace rr::game {

namespace {

using rr::sim::GuestRam;
constexpr uint32_t kAltKind = 0x800541D4, kFightPtr = 0x8005AD4C, kAttackers = 0x800CCAC0;
constexpr uint32_t kCamTargets = 0x80053478; // SLUS 0x800235B0's table, 128 bytes a player
constexpr uint32_t kViewArray = 0x800CD898, kViewStride = 1132;
constexpr uint32_t kRecoverGp = 0x8005AC8C; // SLUS_010.53's gp (race_session.cpp kArenaGp)

uint8_t* Raw(uint8_t* ram, uint32_t a, uint32_t n) {
    if (a < 0x80000000u || static_cast<uint64_t>(a) + n > 0x80200000ull) return nullptr;
    return ram + (a - 0x80000000u);
}

struct NoCameraSeams final : rr::sim::CameraSeams {
    bool GetRCnt(uint32_t, uint32_t&) override { return false; }
    bool ShotSetup(GuestRam&, uint32_t, uint32_t) override { return false; }
    bool SplineSlopes(GuestRam&, uint32_t, uint32_t, uint32_t, int32_t) override { return false; }
    bool RaceOverSignal(uint32_t) override { return false; }
};

// The stance sink of the AI stack's push / pop: the stance event through the dispatcher.
struct Sink final : rr::sim::AiStanceSink {
    ProductRecover& p;
    bool ok = true;
    explicit Sink(ProductRecover& x) : p(x) {}
    void PlayIdleStance(uint16_t event, uint32_t rider) override {
        if (!p.StanceEvent(event, rider, 2)) ok = false;
    }
};

bool PushCommand(ProductRecover& p, uint16_t op, uint16_t target, int32_t mode, uint32_t e) {
    GuestRam& g = p.g();
    static const std::vector<uint8_t> noFight(12u * 256u, 0);
    uint8_t* ep = Raw(p.ram(), e, 1096);
    const uint32_t R = ep != nullptr ? g.U32(e + 0x354u) : 0u;
    uint8_t* rider = Raw(p.ram(), R, 640);
    if (ep == nullptr || rider == nullptr) return false;
    Sink sink(p);
    rr::sim::AiPushEnv pe;
    pe.raceClock = g.S32(g.U32(rr::sim::kRcGameStatePtr) + 0x10u);
    pe.rider = rider;
    pe.riderAddress = R;
    pe.altKindTable = Raw(p.ram(), kAltKind, 8u * 256u);
    const uint32_t fp = g.U32(kFightPtr);
    pe.fightRecords = fp != 0u ? Raw(p.ram(), fp, 12u * 64u) : noFight.data();
    pe.stance = &sink;
    uint8_t cmd[8] = {static_cast<uint8_t>(op), static_cast<uint8_t>(op >> 8), static_cast<uint8_t>(target),
                      static_cast<uint8_t>(target >> 8), 0, 0, 0, 0};
    rr::sim::AiPushCommand(cmd, mode, ep, pe);
    return sink.ok && !g.Faulted();
}

bool PopCommand(ProductRecover& p, uint32_t e) {
    GuestRam& g = p.g();
    static const std::vector<uint8_t> noFight(12u * 256u, 0);
    uint8_t* ep = Raw(p.ram(), e, 1096);
    if (ep == nullptr) return false;
    const uint32_t R = g.U32(e + 852u);
    Sink sink(p);
    rr::sim::AiPopEnv pe;
    pe.gameState = Raw(p.ram(), g.U32(rr::sim::kRcGameStatePtr), 64);
    pe.riderDef = Raw(p.ram(), g.U32(e + 1084u), 72);
    pe.rider = Raw(p.ram(), R, 640);
    pe.riderAddress = R;
    pe.altKindTable = Raw(p.ram(), kAltKind, 8u * 65536u);
    const uint32_t fp = g.U32(kFightPtr);
    pe.fightRecords = fp != 0u ? Raw(p.ram(), fp, 12u * 256u) : noFight.data();
    pe.attackerMask = reinterpret_cast<const uint16_t*>(Raw(p.ram(), kAttackers, 16));
    pe.stance = &sink;
    if (pe.gameState == nullptr || pe.riderDef == nullptr || pe.altKindTable == nullptr) return false;
    rr::sim::AiPopCommand(ep, pe);
    return sink.ok && !g.Faulted();
}

// Remount 0x800903F4's callees, all native (its re-face 0x80096564 is now this domain's port).
struct RemountAdapter final : rr::sim::RemountCallees {
    ProductRecover& p;
    uint32_t sp;
    RemountAdapter(ProductRecover& x, uint32_t s) : p(x), sp(s) {}
    GuestRam& g() { return p.g(); }
    bool RowsFromHeading(uint32_t B) override {
        rr::sim::RowsFromHeading(g(), B);
        return !g().Faulted();
    }
    bool ResetBikeState(uint32_t B) override { return rr::sim::ResetBikeState(g(), B) && !g().Faulted(); }
    bool ReleaseRiderObject(uint32_t R) override { return rr::sim::ReleaseRiderObject(g(), R) && !g().Faulted(); }
    bool Attach(uint32_t B, uint32_t R, int32_t kind, int32_t seat) override {
        rr::sim::Attach(g(), B, R, kind, seat);
        return !g().Faulted();
    }
    bool Dismount(uint32_t R, int32_t how) override {
        struct D final : rr::sim::DismountCallees {
            ProductRecover& p;
            explicit D(ProductRecover& x) : p(x) {}
            bool StanceEvent(uint32_t ev, uint32_t r, uint32_t pp, uint32_t) override { return p.StanceEvent(ev, r, pp); }
            bool StanceLeave(uint32_t ev, uint32_t r, uint32_t pp, uint32_t) override {
                uint32_t v0 = 0;
                const uint32_t a[3] = {ev, r, pp};
                return p.Call(0x800C4500u, a, 3, 0x801FE000u, v0);
            }
            bool AnimStop(uint32_t a, uint32_t) override {
                uint32_t v0 = 0;
                return p.Call(0x8005BE0Cu, &a, 1, 0x801FE000u, v0);
            }
        } d(p);
        return rr::sim::RiderDismount(g(), R, how, sp - 48u, d) && !g().Faulted();
    }
    bool ClearCommands(uint32_t B) override {
        uint8_t* ep = Raw(p.ram(), B, 1096);
        if (ep == nullptr) return false;
        rr::sim::AiClearCommands(ep);
        return true;
    }
    bool PushCommand(uint16_t op, uint16_t target, int32_t mode, uint32_t B) override {
        return rr::game::PushCommand(p, op, target, mode, B);
    }
    bool ReFace(uint32_t B) override {
        uint32_t v0 = 0;
        return p.Call(rr::sim::kReFaceFn, &B, 1, sp - 48u, v0);
    }
    bool ViewReset(uint32_t view) override {
        uint32_t v0 = 0;
        return p.Call(rr::sim::kRcSpringResetFn, &view, 1, sp - 48u, v0);
    }
    bool PlayerVoice(uint32_t h, int32_t a1) override {
        uint32_t v0 = 0;
        const uint32_t a[2] = {h, static_cast<uint32_t>(a1)};
        return p.Call(rr::sim::kRcVoiceFn, a, 2, sp - 48u, v0);
    }
    bool PlayerBind(uint32_t h, uint32_t B) override {
        uint32_t v0 = 0;
        const uint32_t a[2] = {h, B};
        return p.Call(rr::sim::kRcCamTargetFn, a, 2, sp - 48u, v0);
    }
};

// EndRace 0x80092C7C's three callees, PORTED (spine.h), as the session's ProductTailCallees runs them.
struct EndRaceAdapter final : rr::sim::EndRaceCalls, rr::sim::StampResultCallees {
    ProductRecover& p;
    uint32_t bike, view;
    bool ok = true;
    EndRaceAdapter(ProductRecover& x, uint32_t b, uint32_t v) : p(x), bike(b), view(v) {}
    void StampResult() override { ok = rr::sim::StampResult(p.g(), bike, *this) && ok; }
    void WipeoutEffect() override { ok = rr::sim::ResetBikeState(p.g(), bike) && ok; }
    void ViewEvent(int32_t reason) override { rr::sim::ViewEvent(p.g(), view, static_cast<uint32_t>(reason)); }
    bool StanceEvent(uint32_t ev, uint32_t rider, uint32_t pp) override { return p.StanceEvent(ev, rider, pp); }
    bool ClearCommands(uint32_t e) override {
        uint8_t* ep = Raw(p.ram(), e, 1096);
        if (ep == nullptr) return false;
        rr::sim::AiClearCommands(ep);
        return true;
    }
    bool PushCommand(uint16_t op, uint16_t target, int32_t mode, uint32_t e) override {
        return rr::game::PushCommand(p, op, target, mode, e);
    }
};

// FollowStance 0x800C45D8 lives in the presentation layer: its seams are the session's rider seams, the
// stance event, the launch / knock-off (the PORTED RiderLayer on the same seams) and one object sound.
struct PresentAdapter final : rr::sim::PresentSeams {
    ProductRecover& p;
    rr::sim::RiderSeams& s;
    PresentAdapter(ProductRecover& x, rr::sim::RiderSeams& ss) : p(x), s(ss) {}
    uint32_t TransitionCapture(uint32_t a, uint32_t op, uint32_t blend) override { return s.TransitionCapture(a, op, blend); }
    uint32_t ApplyFrame(uint32_t a) override { return s.ApplyFrame(a); }
    uint32_t CombatLeave(uint32_t r, uint32_t cur, uint32_t ev, uint32_t pp) override { return s.CombatLeave(r, cur, ev, pp); }
    uint32_t CombatEnter(uint32_t r, uint32_t cur, uint32_t ev, uint32_t pp) override { return s.CombatEnter(r, cur, ev, pp); }
    uint32_t StanceEvent(uint32_t ev, uint32_t r, uint32_t pp) override {
        rr::sim::StanceLayer layer(p.g(), s);
        return layer.Event(ev, r, pp);
    }
    uint32_t RiderLaunch(uint32_t r) override {
        rr::sim::RiderLayer layer(p.g(), p.ram(), s);
        return layer.Launch(r);
    }
    uint32_t RiderKnockOff(uint32_t r) override {
        rr::sim::RiderLayer layer(p.g(), p.ram(), s);
        return layer.KnockOff(r);
    }
    uint32_t ObjectSound(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override { return 0; }
};

} // namespace

std::string RecoverFnName(uint32_t fn) {
    switch (fn) {
    case rr::sim::kRiderRecoverFn: return "RiderRecover 0x80092E04 (op 18)";
    case rr::sim::kWalkTargetFn: return "WalkTarget 0x80098A50";
    case rr::sim::kWalkPivotFn: return "WalkPivot 0x80098F2C";
    case rr::sim::kWalkTurnFn: return "WalkTurn 0x80099710";
    case rr::sim::kWalkStepFn: return "WalkStep 0x8009989C";
    case rr::sim::kReSeatFn: return "ReSeat 0x8009277C";
    case rr::sim::kClimbDoneFn: return "ClimbDone 0x80092AD4";
    case rr::sim::kReFaceFn: return "ReFace 0x80096564";
    case rr::sim::kFreeFarRiderFn: return "FreeFarRider 0x8008CC94";
    case rr::sim::kFallRiderOffTickFn: return "RiderOffTick 0x8008F138";
    case rr::sim::kFallRiderSyncFn: return "RiderSync 0x8008DF74";
    case rr::sim::kFallRiderSettleFn: return "RiderSettle 0x8009246C";
    case rr::sim::kRiderOnGroundFn: return "RiderOnGround 0x80097BCC";
    case rr::sim::kGroundSlideFn: return "GroundSlide 0x8008F754";
    case rr::sim::kGroundGetUpFn: return "GroundGetUp 0x8008FD5C";
    case rr::sim::kGroundWalkControlFn: return "GroundWalkControl 0x8009926C";
    case rr::sim::kGroundWalkRateFn: return "GroundWalkRate 0x800986D0";
    case rr::sim::kSetOp18Fn: return "SetOp18 0x8009A038";
    case rr::sim::kRiderGroundStepFn: return "RiderGroundStep 0x8008F404";
    case rr::sim::kRiderAirStepFn: return "RiderAirStep 0x800976C4";
    case rr::sim::kRiderLandFn: return "RiderLand 0x800B208C (the rider meets the ground)";
    case rr::sim::kDownWalkEntry: return "the rider pass's crashed-down walk [0x8007DDF4, 0x8007E804)";
    case rr::sim::kRiderAirHitFn: return "RiderAirHit 0x800A966C";
    case rr::sim::kJunctionMarginFn: return "JunctionMargin SLUS 0x8003E338";
    case rr::sim::kRoadEdgeProbeFn: return "RoadEdgeProbe SLUS 0x80038550";
    case rr::sim::kRcVoiceFn: return "the rider-off voice SLUS 0x80018440";
    case rr::sim::kRcJailbreakFn: return "RASHCDG 0x800CA05C (Jailbreak phase 2)";
    default: break;
    }
    char b[32];
    std::snprintf(b, sizeof(b), "0x%08X", fn);
    return b;
}

ProductRecover::ProductRecover(uint8_t* ram, uint32_t gp, const RecoverProductHooks& hooks, RecoverCounts& counts)
    : ram_(ram), g_(ram, gp), hooks_(hooks), counts_(counts) {
    t_ = hooks.tables != nullptr ? *hooks.tables : rr::sim::RecoverTables(ram, g_);
}

bool ProductRecover::Refuse(uint32_t fn, const char* why) {
    ++counts_.refused[fn];
    if (hooks_.seam) hooks_.seam(RecoverFnName(fn) + ": " + why);
    return false;
}

bool ProductRecover::StanceEvent(uint32_t ev, uint32_t r, uint32_t p) {
    if (hooks_.riderSeams == nullptr) return Refuse(rr::sim::kRcStanceEventFn, "no rider seams");
    rr::sim::StanceLayer layer(g_, *hooks_.riderSeams);
    layer.Event(ev, r, p);
    ++counts_.calls[rr::sim::kRcStanceEventFn];
    return !layer.Failed() && !g_.Faulted();
}

bool ProductRecover::Call(uint32_t fn, const uint32_t* a, int n, uint32_t sp, uint32_t& v0) {
    namespace s = rr::sim;
    GuestRam& g = g_;
    auto A = [&](int i) { return i < n ? a[i] : 0u; };
    auto I = [&](int i) { return static_cast<int32_t>(A(i)); };
    v0 = 0;
    ++counts_.calls[fn];
    s::RoadRuntimeNative road;
    bool ok = true;
    switch (fn) {
    // ---- this domain's own ports
    case s::kWalkTargetFn: ok = s::WalkTarget(g, A(0), A(1), A(2), A(3), sp, t_, *this, v0); break;
    case s::kWalkPivotFn: ok = s::WalkPivot(g, A(0), A(1), A(2), I(3), I(4), sp, t_); break;
    case s::kWalkTurnFn: ok = s::WalkTurn(g, A(0), A(1), A(2), I(3), sp, t_, *this); break;
    case s::kWalkStepFn: ok = s::WalkStep(g, A(0), A(1), A(2), I(3), I(4), I(5), sp, t_); break;
    case s::kRiderRecoverFn: ok = s::RiderRecover(g, A(0), I(1), sp, t_, *this); break;
    case s::kReSeatFn: ok = s::ReSeat(g, A(0), sp, *this); break;
    case s::kClimbDoneFn: ok = s::ClimbDone(g, A(0), sp, t_, *this); break;
    case s::kReFaceFn: ok = s::ReFace(g, A(0), sp, t_, *this); break;
    case s::kFreeFarRiderFn: ok = s::FreeFarRider(g, sp, *this, v0); break;
    case s::kFallRiderOffPassFn: ok = s::RiderOffPass(g, I(0), sp, t_, *this); break;
    case s::kFallRiderOffTickFn: ok = s::RiderOffTick(g, A(0), I(1), sp, t_, *this); break;
    case s::kFallPoseAFn: ok = s::PoseA(g, A(0), A(1), sp, t_); break;
    case s::kFallPoseBFn: ok = s::PoseB(g, A(0), A(1), sp, t_); break;
    case s::kFallPoseCFn: ok = s::PoseC(g, A(0), A(1), sp, t_); break;
    case s::kFallPoseDFn: ok = s::PoseD(g, A(0), A(1), sp, t_); break;
    case s::kFallPoseEFn: ok = s::PoseE(g, A(0), A(1), sp, t_); break;
    case s::kFallRiderSyncFn: ok = s::RiderSync(g, A(0), sp, t_, *this); break;
    case s::kFallRiderSettleFn: ok = s::RiderSettle(g, A(0), sp, t_, *this); break;
    case s::kFallLaunchLiftFn: ok = s::LaunchLift(g, A(0), I(1), I(2), I(3), sp, t_); break;
    case s::kRiderOnGroundFn: ok = s::RiderOnGround(g, A(0), I(1), sp, t_, *this); break;
    case s::kGroundSlideFn: ok = s::GroundSlide(g, A(0), I(1), A(2), sp, t_, *this); break;
    case s::kGroundGetUpFn: ok = s::GroundGetUp(g, A(0), sp, t_, *this, v0); break;
    case s::kGroundWalkControlFn: ok = s::GroundWalkControl(g, A(0), I(1), sp, t_, *this); break;
    case s::kGroundWalkRateFn: ok = s::GroundWalkRate(g, A(0), sp, t_, *this, v0); break;
    case s::kSetOp18Fn: ok = s::SetOp18(g, A(0), sp, t_, *this); break;
    case s::kRiderGroundStepFn: ok = s::RiderGroundStep(g, A(0), I(1), sp, t_, *this); break;
    case s::kRiderAirStepFn: ok = s::RiderAirStep(g, A(0), I(1), sp, t_, *this); break;
    case s::kRiderAirHitFn: ok = s::RiderAirHit(g, A(0), A(1), I(2), sp, t_, *this, v0); break;
    case s::kPedHitFn: ok = s::PedHit(g, A(0), A(1), I(2), A(3), A(4), A(5), sp, t_, *this, v0); break;
    case s::kRiderLaunchFn: { // RASHCDG 0x80091468, PORTED (stance.h), PedHit's kind-5 arm
        if (hooks_.riderSeams == nullptr) return Refuse(fn, "no rider seams");
        s::RiderLayer layer(g, ram_, *hooks_.riderSeams);
        v0 = layer.Launch(A(0));
        ok = !layer.Failed();
        break;
    }
    case s::kJunctionMarginFn: ok = s::JunctionMargin(g, A(0), sp, t_, *this, v0); break;
    case s::kRoadEdgeProbeFn: ok = s::RoadEdgeProbe(g, A(0), I(1), I(2), A(3), A(4), sp, t_, *this, v0); break;
    case s::kRiderLandFn: ok = s::RiderLand(g, A(0), sp, t_, *this); break;
    case s::kDownWalkEntry: ok = s::RiderPassDownWalk(g, I(0), sp, t_, *this); break;   // a region, sp = the pass's frame
    case s::kAirCornerMinFn: v0 = s::CornerMin(g, A(0), A(1), A(2), A(3), A(4)); break;
    case s::kAirImpulseFn: s::ApplyImpulse(g, A(0), A(1), I(2)); break;
    case s::kAirSoundFn:
        if (hooks_.sound) hooks_.sound(I(0), I(1), I(2), I(3));
        break;
    case s::kAirPadRumbleFn: // PORTED (collision.h), run with the PORTED motors (rumble_product.h)
        if (rr::game::RumbleOn()) {
            ok = rr::game::ProductPadRumble(g, A(0), A(1), I(2), I(3), I(4), sp);
            break;
        }
        if (hooks_.seam) hooks_.seam("RASHCDG 0x800B658C PadRumble is not run by the product (no pad motors)");
        break;
    // ---- ported in other domains, run natively on the same arena
    case s::kRcRouteBindFn: v0 = s::RouteBind(g, A(0), I(1), A(2), sp, road); break;
    case s::kRcRoadPositionFn: s::RoadPosition(g, A(0), A(1), A(2), sp); break;
    case s::kRcZonesFn: v0 = s::RoadsideZones(g, A(0), sp); break;
    case s::kFallRoadRebindFn: v0 = s::RoadRebind(g, A(0), I(1), sp, road); break;
    case 0x8003EB58u: v0 = static_cast<uint32_t>(s::NodeWedge(g, A(0), A(1), A(2), A(3), I(4), sp)); break;
    case 0x80037A30u: v0 = static_cast<uint32_t>(s::RoadNeighboursForward(g, A(0), A(1), A(2), A(3), I(4), sp)); break;
    case 0x800394F0u: v0 = static_cast<uint32_t>(s::RoadNextObjectMissing(g, A(0), I(1))); break;
    case s::kRcResetBikeFn: ok = s::ResetBikeState(g, A(0)); break;
    case s::kRcReleaseObjFn: ok = s::ReleaseRiderObject(g, A(0)); break;
    case s::kRcViewSlotFn: v0 = s::ViewSlot(g, A(0), A(1)); break;
    case s::kRcAttachFn: s::Attach(g, A(0), A(1), I(2), I(3)); break;
    case s::kRcPoolReleaseFn: ok = s::PoolRelease(g, A(0), I(1)); break;
    case s::kRcCamTargetFn: g.W32(kCamTargets + (A(0) << 7) + 4u, A(1)); break;   // SLUS 0x800235B0, 6 instructions
    case s::kRcPopCommandFn: ok = PopCommand(*this, A(0)); break;
    case s::kFallBuildObbAltFn: {
        uint8_t* ep = Raw(ram_, A(0), 1096);
        const uint8_t* kinds = Raw(ram_, kAltKind, 8u * 256u);
        if (ep == nullptr || kinds == nullptr) return Refuse(fn, "the record is not in the arena");
        // the owner word is read only for a pool-0 entity (0x8008BA6C) - peeked without a fault
        const uint32_t owner = g.U32(A(0) + 852u);
        const uint8_t* op = Raw(ram_, owner + 604u, 4);
        int32_t rev = 0;
        if (op != nullptr) rev = static_cast<int32_t>(op[0] | (op[1] << 8) | (op[2] << 16) | (static_cast<uint32_t>(op[3]) << 24));
        g.ClearFault();
        s::BuildObbAlt(rr::sim::EntityView(ep), kinds, rev);
        break;
    }
    case s::kFallGroundQueryFn: {
        const s::GroundResult r = s::GroundQuery(g, A(0), A(1), A(2), A(3), A(4), t_.rsqrt);
        if (r.declined) return Refuse(fn, "GroundQuery (PORTED) declined a polygon it cannot read");
        v0 = r.value;
        break;
    }
    case s::kAirRowsFromUpFn: ok = s::RowsFromUp(g, A(0), t_); break;
    case s::kAirBounceFn: {
        bool bok = true;
        v0 = static_cast<uint32_t>(s::Bounce(g, A(0), A(1), A(2), A(3), I(4), I(5), I(6), t_, bok));
        ok = bok;
        break;
    }
    case s::kRcSpringResetFn:
    case s::kGrCameraResetFn: {
        NoCameraSeams none;
        s::CameraPort port(g, t_, none, road);
        if (fn == s::kRcSpringResetFn) port.SpringReset(A(0));
        else port.ResetFlags(A(0));
        ok = !port.Failed();
        break;
    }
    case s::kRcRemountFn: {
        RemountAdapter ra(*this, sp);
        ok = s::Remount(g, A(0), I(1), ra);
        break;
    }
    case s::kRcEndRaceFn: {
        const uint32_t e = A(0);
        const uint32_t h = g.U16(e + 0xACu);
        const uint32_t view = kViewArray + kViewStride * h;
        EndRaceAdapter calls(*this, e, view);
        s::EndRaceEnv ee;
        ee.entity = Raw(ram_, e, 1096);
        ee.riderDef = Raw(ram_, g.U32(e + 1084u), 72);
        ee.owner = Raw(ram_, g.U32(e + 852u), 628);
        ee.view = Raw(ram_, view, 0x310);
        ee.gameState = Raw(ram_, g.U32(s::kRcGameStatePtr), 64);
        ee.postDelay = reinterpret_cast<int32_t*>(Raw(ram_, 0x8005B230u, 4));
        ee.calls = &calls;
        ok = s::EndRace(I(1), ee) && calls.ok;
        break;
    }
    case s::kGrEffectBurstFn: {
        s::SpineIo io;
        io.rootCounter = hooks_.rootCounter ? hooks_.rootCounter() : 0u;
        s::EffectBurst(g, A(0), A(1), A(2), A(3), io);
        break;
    }
    // ---- the stance layer and the animation machine (the session's rider seams)
    case s::kRcStanceEventFn: --counts_.calls[fn]; ok = StanceEvent(A(0), A(1), A(2)); break;
    case s::kGrClipDoneFn:
    case s::kGrRangedStartFn:
    case s::kRcBankSwitchFn:
    case s::kGrSetRiderStateFn:
    case 0x800C4500u:   // StanceLeave (RiderDismount's)
    case 0x8005BE0Cu: { // the animation stop (RiderDismount's)
        if (hooks_.riderSeams == nullptr) return Refuse(fn, "no rider seams");
        s::StanceLayer layer(g, *hooks_.riderSeams);
        if (fn == s::kGrClipDoneFn) v0 = layer.anim().ClipDone(A(0));
        else if (fn == s::kGrRangedStartFn) {
            const uint32_t st = sp + 16u;   // RangedStart's last three o32 arguments, stored by the caller
            v0 = layer.anim().RangedStart(A(0), A(1), A(2), A(3), n > 4 ? A(4) : g.U32(st),
                                          n > 5 ? A(5) : g.U32(st + 4u), n > 6 ? A(6) : g.U32(st + 8u));
        } else if (fn == s::kRcBankSwitchFn) v0 = layer.anim().BankSwitch(A(0), A(1));
        else if (fn == s::kGrSetRiderStateFn) v0 = layer.SetRiderState(A(0), A(1), A(2));
        else if (fn == 0x800C4500u) v0 = layer.Leave(A(0), A(1), A(2));
        else layer.anim().Stop(A(0));
        ok = !layer.Failed();
        break;
    }
    case s::kGrFollowStanceFn: {
        if (hooks_.riderSeams == nullptr) return Refuse(fn, "no rider seams");
        PresentAdapter pa(*this, *hooks_.riderSeams);
        s::PresentLayer layer(g, pa);
        v0 = layer.FollowStance(A(0), A(1));
        ok = !layer.Failed();
        break;
    }
    case s::kRcVoiceFn: // SLUS 0x80018440 RiderOffSound, PORTED on the session's rider seams (takedown_product.h)
        if (hooks_.riderSeams == nullptr) return Refuse(fn, "no rider seams");
        v0 = hooks_.riderSeams->RiderOffSound(A(0), A(1));
        break;
    // ---- the pedestrians (peds.h): the pedestrian passes and spawner, the animation starts they reach, the voice
    case s::kPedsPassFn: ok = s::PedPass(g, I(0), sp, t_, *this); break;
    case s::kPedsReleaseFn: ok = s::PedRelease(g, I(0), sp, t_, *this); break;
    case s::kPedSpawnFn: ok = s::PedSpawn(g, A(0), A(1), A(2), sp, t_, *this, v0); break;
    case s::kPedLoopStartFn:
    case s::kPedHardStartFn:
    case s::kPedHoldFn:
    case s::kPedResumeFn: {
        if (hooks_.riderSeams == nullptr) return Refuse(fn, "no rider seams");
        s::AnimMachine m(g, *hooks_.riderSeams);
        const uint32_t ex = n > 4 ? A(4) : g.U32(sp + 16u); // the starts' fifth o32 argument
        if (fn == s::kPedLoopStartFn) v0 = m.LoopStart(A(0), A(1), A(2), A(3), ex);
        else if (fn == s::kPedHardStartFn) v0 = m.HardStart(A(0), A(1), A(2), A(3), ex);
        else if (fn == s::kPedHoldFn) v0 = m.StopIfPlaying(A(0));
        else v0 = m.Resume(A(0));
        ok = !m.Failed();        break;
    }
    case s::kPedVoiceFn: { // SLUS 0x8001B44C PedVoice, PORTED (partners.h, row partners_ped_voice)
        if (!StrikeOn()) { // RRJB_STRIKE=off: the negative control, the named seam
            if (hooks_.seam) hooks_.seam("SLUS 0x8001B44C the pedestrian's voice is not ported: the cry of a running pedestrian is absent");
            break;
        }
        // PlaySound3D is its only callee: the session's PORTED emitter. PedPass passes flag 0 (RASHCDG 0x800CAD28
        // `move a3,zero`), so the original's call returns at once and plays nothing - and so does the port.
        struct Voice final : rr::sim::CollisionCallees {
            const RecoverProductHooks& h;
            size_t played = 0;
            explicit Voice(const RecoverProductHooks& x) : h(x) {}
            bool PlaySound3D(int32_t x, int32_t z, int32_t id, int32_t bank) override {
                ++played;
                if (h.sound) h.sound(x, z, id, bank);
                return true;
            }
            bool ReleaseContact(uint32_t) override { return false; }
            bool Rumble(uint32_t, uint32_t, int32_t, int32_t, int32_t, uint32_t) override { return false; }
            bool Unported(uint32_t, const uint32_t*, int, uint32_t, uint32_t&) override { return false; }
        } voice(hooks_);
        ok = rr::sim::PedVoice(g, I(0), I(1), A(2), A(3), voice);
        ++StrikeCounters().pedVoices;
        StrikeCounters().pedVoiced += voice.played;
        break;
    }
    // ---- NOT ported
    case s::kRcJailbreakFn:                                            // JailBoard 0x800CA05C (jail_session.cpp)
        if (!hooks_.jailBoard) return Refuse(fn, "no Jailbreak hook (reached only in Jailbreak phase 2)");
        ok = hooks_.jailBoard(A(0), sp);
        break;
    default: return Refuse(fn, "no native port is wired for this callee");
    }
    if (g.Faulted()) return Refuse(fn, "met an address the console would fault on");
    if (!ok) return Refuse(fn, "the port refused");
    return true;
}

namespace {
bool Run(uint8_t* ram, uint32_t gp, uint32_t fn, const uint32_t* a, int n, uint32_t sp, const RecoverProductHooks& h,
         RecoverCounts& counts, uint32_t* v0) {
    ProductRecover p(ram, gp, h, counts);
    uint32_t r = 0;
    const bool ok = p.Call(fn, a, n, sp, r);
    if (v0 != nullptr) *v0 = r;
    return ok;
}
} // namespace

bool RunRiderRecover(uint8_t* ram, uint32_t gp, uint32_t e, int32_t dt, uint32_t sp, const RecoverProductHooks& h,
                     RecoverCounts& counts) {
    const uint32_t a[2] = {e, static_cast<uint32_t>(dt)};
    return Run(ram, gp, rr::sim::kRiderRecoverFn, a, 2, sp, h, counts, nullptr);
}

bool RunClimbDone(uint8_t* ram, uint32_t gp, uint32_t B, uint32_t sp, const RecoverProductHooks& h,
                  RecoverCounts& counts) {
    return Run(ram, gp, rr::sim::kClimbDoneFn, &B, 1, sp, h, counts, nullptr);
}

bool RunRiderOffPass(uint8_t* ram, uint32_t gp, int32_t dt, uint32_t sp, const RecoverProductHooks& h,
                     RecoverCounts& counts) {
    const uint32_t a[1] = {static_cast<uint32_t>(dt)};
    return Run(ram, gp, rr::sim::kFallRiderOffPassFn, a, 1, sp, h, counts, nullptr);
}

bool RunRiderGroundLoop(uint8_t* ram, uint32_t gp, int32_t dt, uint32_t sp, const RecoverProductHooks& h,
                        RecoverCounts& counts, const std::function<bool(uint32_t e)>& migrate) {
    // [0x8007DCEC, 0x8007DDD4): per pool-0 slot, the migration (0x8007DD28), then the rider when its
    // mount state is >= 2 and it is live (0x8007DD60), then a passenger's the same way (0x8007DDBC).
    ProductRecover p(ram, gp, h, counts);
    GuestRam& g = p.g();
    const uint32_t pool = rr::sim::kRcPoolTable;
    int32_t n = g.S32(g.U32(pool + 12u));
    uint32_t e = g.U32(pool);
    bool ok = true;
    while (n >= 0) {
        if ((g.U32(e + 568u) & 0x08001800u) && migrate) ok = migrate(e) && ok;
        const uint32_t r = g.U32(e + 852u);
        uint32_t v0 = 0;
        if (!(g.U32(r + 604u) < 2u) && g.S16(r + 320u) != 0) {
            const uint32_t a[2] = {r, static_cast<uint32_t>(dt)};
            ok = p.Call(rr::sim::kRiderGroundStepFn, a, 2, sp, v0) && ok;
        }
        if (g.U8(g.U32(e + 852u) + 572u) & 0x10u) {
            const uint32_t pr = g.U32(g.U32(e + 856u) + 852u);
            if (!(g.U32(pr + 604u) < 2u) && g.S16(pr + 320u) != 0) {
                const uint32_t a[2] = {pr, static_cast<uint32_t>(dt)};
                ok = p.Call(rr::sim::kRiderGroundStepFn, a, 2, sp, v0) && ok;
            }
        }
        e += g.U32(pool + 4u);
        --n;
    }
    return ok;
}

// ---- RaceSession's side (race_session.h): the hooks run the dispatcher on the session's arena.
bool RaceSession::RecoverCall(uint32_t fn, std::initializer_list<uint32_t> args, uint32_t sp, rr::sim::RiderSeams& seams,
                              uint32_t* v0) {
    RecoverProductHooks h;
    h.riderSeams = &seams;
    const rr::sim::BikeTables t = SessionTables();
    h.tables = &t;
    h.rootCounter = [this]() {
        fxCounter_ = fxCounter_ * 1103515245u + 12345u;
        return (fxCounter_ >> 16) & 0xFFFFu;
    };
    h.seam = [this](const std::string& s) { NoteSeam(s + " (the rider-recovery domain, recover_race.h)"); };
    h.sound = [this](int32_t x, int32_t z, int32_t id, int32_t bank) { sounds_.PlaySound3D(x, z, id, bank); };
    h.jailBoard = [this](uint32_t late, uint32_t sp) { return ModeJailBoard(late, sp); }; // jail_session.cpp
    const bool ok = RunRecoverCall(arena_.Ram(), kRecoverGp, fn, args, sp, h, recoverCounts_, v0);
    if (!ok) NoteSeam("the rider-recovery domain refused " + RecoverFnName(fn) + ": the arena keeps what it wrote");
    return ok;
}

bool RaceSession::RecoverRiderLoop(int32_t dt, uint32_t sp, rr::sim::RiderSeams& seams,
                                   const std::function<bool(uint32_t e)>& migrate) {
    RecoverProductHooks h;
    h.riderSeams = &seams;
    const rr::sim::BikeTables t = SessionTables();
    h.tables = &t;
    h.rootCounter = [this]() {
        fxCounter_ = fxCounter_ * 1103515245u + 12345u;
        return (fxCounter_ >> 16) & 0xFFFFu;
    };
    h.seam = [this](const std::string& s) { NoteSeam(s + " (the rider-recovery domain, recover_race.h)"); };
    h.sound = [this](int32_t x, int32_t z, int32_t id, int32_t bank) { sounds_.PlaySound3D(x, z, id, bank); };
    h.jailBoard = [this](uint32_t late, uint32_t sp) { return ModeJailBoard(late, sp); }; // jail_session.cpp
    const bool ok = RunRiderGroundLoop(arena_.Ram(), kRecoverGp, dt, sp, h, recoverCounts_, migrate);
    if (!ok) NoteSeam("the rider pass's rider loop [0x8007DCEC, 0x8007DDD4) (PORTED) refused a rider");
    return ok;
}

rr::sim::BikeTables RaceSession::SessionTables() const {
    rr::sim::BikeTables t;
    t.sincos = tables_.sincos.data();
    t.asin = tables_.atanU16.data();
    t.atan = tables_.atan.data();
    t.rsqrt = tables_.rsqrtTable.data();
    t.sqrt = tables_.sqrtWindow.data() + 0x800 / 2;
    return t;
}

std::string RaceSession::RecoverTotals() const {
    std::string s = "the rider-recovery domain (recover_race.h), native calls by function:";
    for (const auto& [fn, n] : recoverCounts_.calls) s += " " + RecoverFnName(fn) + " x" + std::to_string(n) + ";";
    if (!recoverCounts_.refused.empty()) {
        s += " REFUSED:";
        for (const auto& [fn, n] : recoverCounts_.refused) s += " " + RecoverFnName(fn) + " x" + std::to_string(n) + ";";
    }
    return s;
}

bool RunRecoverCall(uint8_t* ram, uint32_t gp, uint32_t fn, std::initializer_list<uint32_t> args, uint32_t sp,
                    const RecoverProductHooks& h, RecoverCounts& counts, uint32_t* v0) {
    uint32_t a[12] = {};
    int n = 0;
    for (uint32_t x : args)
        if (n < 12) a[n++] = x;
    return Run(ram, gp, fn, a, n, sp, h, counts, v0);
}

} // namespace rr::game
