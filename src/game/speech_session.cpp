// The riders' voices in the race: the session's side of the
// PORTED speech code of src\game\sim\speech.cpp, which runs in the sound runtime on this arena.
//   * StartSpeech: AUDTAUNT.STR to the sound runtime (SpeechInit and the records it streams);
//   * RiderSpeech: SLUS 0x8001A760 for every caller the product runs - the stance layer's crash line
//     (RiderLaunch B..E), BikeBikeGate's knock-on (the collision pass), FightSteer's and
//     AiChooseCommand's taunts, and the pad's L2 (TauntPad); a player's taunt's provocation pushes the
//     PORTED AiPushCommand with the caller's stance seams;
//   * TauntPad: the pad reader's L2 arm SLUS 0x8001D548..0x8001D5A0 for player 1, transcribed.
#include <cstring>
#include <string>
#include <vector>

#include "game/race_session.h"
#include "game/sim/ai.h"
#include "game/sim/pad_reader.h"
#include "game/sim/race.h"
#include "game/sim/road_runtime.h"
#include "game/sim/stance.h"

namespace rr::game {
namespace {

using rr::sim::GuestRam;

constexpr uint32_t kGp = 0x8005AC8C;          // SLUS_010.53's gp (race_session.cpp kArenaGp)
constexpr uint32_t kSpeechSp = 0x801FDC00;    // OURS: the stack a caller without one makes the call at
constexpr uint32_t kPadRecord = 0x800D7128;   // player 1's frame-stable pad record
constexpr uint32_t kGameStatePtr = 0x8005B2F8, kFightRecPtr = 0x8005AD4C, kAltKind = 0x800541D4;

uint8_t* Raw(uint8_t* ram, uint32_t a, uint32_t n) {
    if (a < 0x80000000u || static_cast<uint64_t>(a) + n > 0x80200000ull) return nullptr;
    return ram + (a - 0x80000000u);
}

int32_t ReadS32(const uint8_t* p, uint32_t off) {
    int32_t v = 0;
    std::memcpy(&v, p + off, 4);
    return v;
}

} // namespace

void RaceSession::StartSpeech(const DiscImage& disc) {
    const auto taunt = disc.Find("DATA/AUDTAUNT.STR");
    if (!sounds_.Attached() || !taunt) {
        NoteSeam("the riders' voices do not start: no DATA/AUDTAUNT.STR or no sound arena");
        return;
    }
    const DiscImage* d = &disc;
    const rr::DiscFile file = *taunt;
    auto read = [d, file](int which, uint32_t offset, uint8_t* dst, uint32_t bytes) {
        if (which != 2 || static_cast<uint64_t>(offset) + bytes > file.size) return false;
        d->ReadForm1(file.lba, offset, dst, bytes);
        return true;
    };
    // SLUS 0x800138E8 ComputePlace (PORTED, race.h) on this arena's bikes, as the frame's place pool
    // resolves them (race_session.cpp Frame): SpeechSlotFind asks for the player's place band.
    auto place = [this](uint32_t bike, int32_t mode, uint32_t& out) {
        uint8_t* const ram = arena_.Ram();
        GuestRam g(ram, kGp);
        const int16_t routeArmed = g.S16(rr::sim::kRouteRecordCount);
        std::vector<rr::sim::PlaceNode> pool(bikes_.size());
        for (size_t i = 0; i < bikes_.size(); ++i) {
            pool[i].entity = bikes_[i].entity.data();
            pool[i].riderDef = bikes_[i].riderDef.data();
            rr::sim::RouteBinding b;
            b.routeObject = Raw(ram, static_cast<uint32_t>(ReadS32(bikes_[i].entity.data(), 0x1AC)), 120);
            if (b.routeObject != nullptr) {
                b.firstWord = static_cast<uint32_t>(ReadS32(b.routeObject, 0));
                b.legs = b.routeObject + 20;
                b.legCount = ReadS32(b.routeObject, 0x0C);
            }
            b.routeArmed = routeArmed;
            pool[i].binding = b;
        }
        rr::sim::ComputePlaceEnv env;
        env.gameState = gameState_.data();
        env.liveBikes = *liveBikes_;
        env.raceFlags = raceFlags_;
        env.pool = pool.data();
        env.poolHigh = static_cast<int32_t>(bikes_.size()) - 1;
        env.poolCount = static_cast<int32_t>(bikes_.size());
        for (size_t i = 0; i < bikes_.size(); ++i)
            if (bikes_[i].entityAddress == bike) {
                int32_t p = 0;
                const bool ok = rr::sim::ComputePlace(pool[i].entity, pool[i].riderDef, pool[i].binding, mode, env, &p);
                out = static_cast<uint32_t>(p);
                return ok;
            }
        return false;
    };
    const bool ok = sounds_.StartSpeech(read, static_cast<uint32_t>(file.size), place);
    NoteSeam(ok ? std::string("the riders' voices: the PORTED SpeechInit SLUS 0x8001A424 asked for seven "
                              "AUDTAUNT.STR records; they are read at the first frame and loaded by the PORTED "
                              "SpeechBankLoad into the speech slots (the CD streamer, the SPU heap and the transfer "
                              "are ours); RiderSpeech SLUS 0x8001A760 is PORTED for every caller the product runs")
                : std::string("the riders' voices did not start: SpeechInit (PORTED) refused - ") + sounds_.SpeechFault());
}

bool RaceSession::RiderSpeech(uint32_t h, int32_t crash, uint32_t sp, rr::sim::StanceSeams* seams) {
    if (sp == 0) sp = kSpeechSp;
    uint8_t* const ram = arena_.Ram();
    // RASHCDG 0x800BCA68 AiPushCommand (PORTED, ai.h) for the provocation: the 8-byte record in
    // RiderSpeech's frame read, pushed, written back; its stance request on the caller's stance layer.
    auto push = [this, ram, seams](uint32_t cmdAt, int32_t mode, uint32_t e) {
        GuestRam g(ram, kGp);
        uint8_t* ep = Raw(ram, e, 1096);
        uint8_t* rec = Raw(ram, cmdAt, 8);
        const uint32_t R = g.U32(e + 0x354u);
        const uint32_t fp = g.U32(kFightRecPtr);
        static const std::vector<uint8_t> noFight(12u * 256u, 0);
        const uint8_t* fight = fp != 0u ? Raw(ram, fp, 12u * 64u) : noFight.data();
        if (ep == nullptr || rec == nullptr || fight == nullptr || seams == nullptr) return false;
        struct Sink final : rr::sim::AiStanceSink {
            GuestRam& g;
            rr::sim::StanceSeams& seams;
            bool ok = true;
            Sink(GuestRam& gg, rr::sim::StanceSeams& s) : g(gg), seams(s) {}
            void PlayIdleStance(uint16_t event, uint32_t rider) override {
                rr::sim::StanceLayer layer(g, seams);
                layer.Event(event, rider, 2);
                if (layer.Failed() || g.Faulted()) ok = false;
            }
        } sink(g, *seams);
        rr::sim::AiPushEnv pe;
        pe.raceClock = g.S32(g.U32(kGameStatePtr) + 0x10u);
        pe.rider = Raw(ram, R, 640);
        pe.riderAddress = R;
        pe.altKindTable = Raw(ram, kAltKind, 8u * 256u);
        pe.fightRecords = fight;
        pe.stance = &sink;
        uint8_t c[8];
        std::memcpy(c, rec, 8);
        rr::sim::AiPushCommand(c, mode, ep, pe);
        std::memcpy(rec, c, 8);
        return sink.ok;
    };
    // measurement: the provocation's grudge byte (riderDef +0x10..) of the AI it picked
    std::vector<uint8_t> before(bikes_.size() * 16u);
    for (size_t i = 0; i < bikes_.size(); ++i) std::memcpy(before.data() + 16u * i, bikes_[i].riderDef.data() + 0x10, 16);
    const bool ok = sounds_.Speech(h, crash, sp, push);
    for (size_t i = 0; i < bikes_.size(); ++i)
        if (std::memcmp(before.data() + 16u * i, bikes_[i].riderDef.data() + 0x10, 16) != 0) ++grudges_;
    if (!ok && !sounds_.SpeechFault().empty())
        NoteSeam("SLUS 0x8001A760 RiderSpeech (PORTED) stopped: " + sounds_.SpeechFault() + " - no more voice lines");
    return true; // a void function: the caller carries on either way
}

// The pad reader's L2 arm for player p (SLUS 0x8001D548..0x8001D5A0, inside its on-the-bike branch:
// game_state[0] == 1, the bike under pad control, its rider active and on the bike), transcribed: the
// press code of control 8's slot (the remap table rec+0xB8, +0x20) starts the rider's taunt; while the
// slot is held the rider's +0x228 carries 0x800000. The slot is stamped by FightPadPass's post-processor
// from the repacked word's bit 0 (L2, slot 9); the press code is cleared after the frame by
// RunPadControls (pad_product.h), as the reader's first loop clears it on the console. The arm is the
// player loop's: s2 = the loop's pad record 0x800D7128 + 192p, s3 = its bike *(0x8005B268 + 4p) (our
// listing 0x8001D51C..0x8001D5AC), so player 2's L2 runs it on record 1 and bike 1 (mp_session.cpp).
void RaceSession::TauntPad(rr::sim::StanceSeams& seams, int p) {
    if (p < 0 || static_cast<size_t>(p) >= bikes_.size()) return;
    GuestRam g(arena_.Ram(), kGp);
    const uint32_t gs = g.U32(kGameStatePtr);
    const uint32_t bike = bikes_[static_cast<size_t>(p)].entityAddress;
    const uint32_t rec = kPadRecord + 192u * static_cast<uint32_t>(p);
    if (g.U8(gs) != 1u || (g.U32(bike + 0x230u) & 0x08000000u)) return;
    const uint32_t rider = g.U32(bike + 0x354u);
    if (g.S16(rider + 0x140u) == 0 || !(g.U32(rider + 0x25Cu) < 2u)) return;
    uint32_t handle = 0, v0 = 0;
    if (rr::sim::PadTauntPress(g, rec, bike, handle, v0)) { // PORTED, bench row polish_taunt_arm
        ++taunts_;
        RiderSpeech(handle, 0, kSpeechSp, &seams);
    }
    rr::sim::PadTauntHeld(g, rec, bike);
    if (g.Faulted()) {
        g.ClearFault();
        NoteSeam("the pad reader's L2 arm met an address this arena faults on");
    }
}

} // namespace rr::game
