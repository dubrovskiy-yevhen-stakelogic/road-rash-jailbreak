// Combat in the product: FIGHT.BIN in the arena, the pad handler's
// combat tail, the op-16 arm of the AI command pass and the stance layer's two combat children, all
// running the PORTED fight code of src\game\sim\fight.cpp on the session's guest arena.
#include "game/fight_session.h"
#include "game/rumble_product.h" // the pad motors

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "game/race_session.h"
#include "game/fight_physical.h" // VR physical combat
#include "game/passes_product.h" // who writes the frame clock
#include "game/sim/ai.h"
#include "game/sim/anim.h"
#include "game/sim/fight.h"
#include "game/sim/stance.h"
#include "game/sim/strike.h" // Strike RASHCDG 0x800C1370
#include "game/strike_product.h"
#include "game/weapon_session.h"
#include "game/pause_product.h" // PauseOn
#include "game/loader_product.h" // FightLoad RASHCDI 0x800653E8 PORTED

namespace rr::game {
namespace {

using rr::sim::GuestRam;
namespace F = rr::sim::fight;

constexpr uint32_t kGp = 0x8005AC8C;            // SLUS_010.53's gp (race_session.cpp kArenaGp)
constexpr uint32_t kArenaFight = 0x8000C000;     // OURS: where FIGHT.BIN is put (rr-race: 0x801A6B5C, a cell buffer here)
constexpr uint32_t kFightRecords = 40;           // RASHCDI 0x800653E8 relocates exactly 40 (`slti v0,a1,40`)
constexpr uint32_t kPadRecord = 0x800D7128;      // player 1's frame-stable pad record, 192 bytes
constexpr uint32_t kPadRemap = 0x800526A4;       // SLUS: control -> slot, the record's +0xB8
constexpr uint32_t kSlotMasks = 0x80052658;      // SLUS: the 16 slot masks of the repacked word
constexpr uint32_t kFightSp = 0x801FE800;        // OURS: the stack the fight code's frames sit on
constexpr uint32_t kSqrtTable = 0x800560CC;      // SLUS: Length3's table (SetAimDelta)
constexpr uint32_t kAltKind = 0x800541D4, kAttackers = 0x800CCAC0, kGameStatePtr = 0x8005B2F8;

uint8_t* Raw(uint8_t* ram, uint32_t a, uint32_t n) {
    if (a < 0x80000000u || static_cast<uint64_t>(a) + n > 0x80200000ull) return nullptr;
    return ram + (a - 0x80000000u);
}

// The pad combat gate's counters (FightPadPass): frames the decode was skipped with the rider off the bike,
// frames it ran there anyway (RRJB_PADGATE=off), and the pad players' falls - the rider's mount state leaving
// 0/1 - with those that come less than 240 ticks after the same player's previous fall (one crash, fallen twice).
struct PadGateCounts {
    uint64_t skipped = 0, skippedOther = 0, ungated = 0, falls = 0, refalls = 0;
    uint32_t lastMount[2] = {0, 0};
    int32_t lastFall[2] = {INT32_MIN, INT32_MIN};
};

PadGateCounts& PadGateCount() {
    static PadGateCounts c;
    return c;
}

bool PadGateOn() {
    static const bool on = [] {
        const char* v = std::getenv("RRJB_PADGATE");
        return !(v != nullptr && std::strcmp(v, "off") == 0);
    }();
    return on;
}

void PadGateNoteFall(uint32_t mount, int32_t clock, int p) {
    PadGateCounts& c = PadGateCount();
    const size_t i = p == 0 ? 0u : 1u;
    if (c.lastMount[i] < 2u && mount >= 2u) {
        ++c.falls;
        if (c.lastFall[i] != INT32_MIN && clock - c.lastFall[i] < 240) ++c.refalls;
        c.lastFall[i] = clock;
    }
    c.lastMount[i] = mount;
}

// The fight command pass can put a fallen rider back in a fight stance (mount 1) within the frame: the
// state it leaves is what the next fall is measured from.
void PadGateSeen(uint32_t mount, int p) { PadGateCount().lastMount[p == 0 ? 0u : 1u] = mount; }

// The run's fight totals (the fight damage gate). A blow is an ApplyHit 0x800C17B0 call, which
// stamps the last-blow table (NoteHit 0x800C1C18) on a MISS too; it lands only with ReachTest 0x800C159C's
// bit 0 (the lateral reach), and only a landed blow bumps the attacker's "hits" stat FightStat(me, 2, 0).
struct FightRunTotals {
    uint64_t blows = 0, landed = 0;         // the player's ApplyHit calls / those with the reach bit
    uint64_t rivalsHurt = 0, playerHurt = 0; // riders whose health went down in a fight call (op 16 / op 9)
    uint64_t knockOffs = 0;                  // ... to 0
    int rivalLow = -1;                       // the lowest health a rival was left on by a blow
};

FightRunTotals& FightTotalsCount() {
    static FightRunTotals t;
    return t;
}

void NoteHurt(size_t bike, uint8_t now) {
    FightRunTotals& t = FightTotalsCount();
    ++(bike == 0 ? t.playerHurt : t.rivalsHurt);
    if (now == 0) ++t.knockOffs;
    if (bike != 0 && (t.rivalLow < 0 || now < t.rivalLow)) t.rivalLow = now;
}

} // namespace

std::string FightTotals() {
    const FightRunTotals& t = FightTotalsCount();
    return "the fight totals: the player's blows " + std::to_string(t.blows) + ", landed " +
           std::to_string(t.landed) + ", missed " + std::to_string(t.blows - t.landed) + "; riders hurt: rivals " +
           std::to_string(t.rivalsHurt) + " (lowest health left " + std::to_string(t.rivalLow) + "), the player " +
           std::to_string(t.playerHurt) + "; knocked to 0: " + std::to_string(t.knockOffs) + "\n";
}

std::string PadGateTotals() {
    const PadGateCounts& c = PadGateCount();
    return "the pad combat gate" + std::string(PadGateOn() ? "" : " OFF (RRJB_PADGATE=off)") +
           ": SLUS 0x8001D350's decode skipped " + std::to_string(c.skipped) + " frame(s) with the rider off the bike and " +
           std::to_string(c.skippedOther) + " on it but not racing / not under pad control / inactive, run " +
           std::to_string(c.ungated) + " past the gate; the pad players' falls " + std::to_string(c.falls) + ", of them " +
           std::to_string(c.refalls) + " less than 240 ticks after the previous one\n";
}

// The fight code's callees in the product. The PORTED ones run where they live: the stance event and
// the clip test on the session's StanceLayer / AnimMachine with the rider seams it was handed, the sound
// emitter and queue on the sound runtime, the command stack's push and pop (ai.h) on raw views of the
// arena, SetAimDelta (ai.h). The unported ones are named in the seam list: those with an effect on the
// race (EndRace's phase-3 arm is not reached here: game_state+0x39 is 0; Arrest, the weapon objects,
// the overlay clip) REFUSE - FightCommand then restores the arena - and the two whose only effect is
// outside the simulation (the pad motors, the rider's voice line) are skipped, named and counted.
class ProductFight final : public F::Callees {
public:
    ProductFight(RaceSession* s, GuestRam& g, uint8_t* ram, rr::sim::StanceSeams& seams, SoundRuntime& snd)
        : s_(s), g_(g), ram_(ram), seams_(seams), snd_(snd) {}
    bool refused = false;

    bool StanceEvent(uint32_t ev, uint32_t r, uint32_t p, uint32_t& v0) override {
        rr::sim::StanceLayer layer(g_, seams_);
        v0 = layer.Event(ev, r, p);
        return !layer.Failed() && !g_.Faulted();
    }
    bool ClipDone(uint32_t a, uint32_t& v0) override {
        rr::sim::AnimMachine m(g_, seams_);
        v0 = m.ClipDone(a);
        return !g_.Faulted();
    }
    bool PlaySound3D(int32_t x, int32_t z, int32_t id, int32_t bank) override {
        snd_.PlaySound3D(x, z, id, bank);
        return true;
    }
    bool QueueListenerSound(uint32_t e, int32_t id, int32_t t, int32_t p) override {
        snd_.QueueListenerSound(static_cast<int32_t>(e), id, t, p);
        return true;
    }
    struct Sink final : rr::sim::AiStanceSink {
        ProductFight& f;
        bool ok = true;
        explicit Sink(ProductFight& x) : f(x) {}
        void PlayIdleStance(uint16_t event, uint32_t rider) override {
            uint32_t v0 = 0;
            if (!f.StanceEvent(event, rider, 2, v0)) ok = false;
        }
    };
    bool PushCommand(uint8_t cmd[8], uint32_t, int32_t mode, uint32_t e) override { // RASHCDG 0x800BCA68, PORTED
        uint8_t* ep = Raw(ram_, e, 1096);
        const uint32_t R = g_.U32(e + 0x354u);
        const uint8_t* fight = Raw(ram_, g_.U32(F::kFightRecPtr), 12u * 64u);
        if (ep == nullptr || fight == nullptr) return false;
        Sink sink(*this);
        rr::sim::AiPushEnv pe;
        pe.raceClock = g_.S32(g_.U32(kGameStatePtr) + 0x10u);
        pe.rider = Raw(ram_, R, 640);
        pe.riderAddress = R;
        pe.altKindTable = Raw(ram_, kAltKind, 8u * 256u);
        pe.fightRecords = fight;
        pe.stance = &sink;
        rr::sim::AiPushCommand(cmd, mode, ep, pe);
        return sink.ok && !g_.Faulted();
    }
    bool PopCommand(uint32_t e) override { // RASHCDG 0x800BC8DC, PORTED
        uint8_t* ep = Raw(ram_, e, 1096);
        const uint32_t R = g_.U32(e + 0x354u);
        Sink sink(*this);
        rr::sim::AiPopEnv pe;
        pe.gameState = Raw(ram_, g_.U32(kGameStatePtr), 0x60);
        pe.riderDef = Raw(ram_, g_.U32(e + 0x43Cu), 72);
        pe.rider = Raw(ram_, R, 640);
        pe.riderAddress = R;
        pe.altKindTable = Raw(ram_, kAltKind, 8u * 256u);
        pe.fightRecords = Raw(ram_, g_.U32(F::kFightRecPtr), 12u * 64u);
        pe.attackerMask = reinterpret_cast<const uint16_t*>(Raw(ram_, kAttackers, 16));
        pe.stance = &sink;
        if (ep == nullptr || pe.gameState == nullptr || pe.riderDef == nullptr || pe.fightRecords == nullptr)
            return false;
        rr::sim::AiPopCommand(ep, pe);
        return sink.ok && !g_.Faulted();
    }
    bool SetAimDelta(uint32_t e, uint32_t, int32_t scalar) override { // RASHCDG 0x80093CAC, PORTED
        uint8_t* ep = Raw(ram_, e, 1096);
        const int16_t* sq = reinterpret_cast<const int16_t*>(Raw(ram_, kSqrtTable, 2));
        if (ep == nullptr || sq == nullptr) return false;
        rr::sim::SetAimDelta(rr::sim::EntityView(ep), nullptr, reinterpret_cast<const int16_t*>(ep + 0x368), &scalar, 1,
                             sq);
        return true;
    }
    bool EndRace(uint32_t, int32_t) override { return Refuse("RASHCDG 0x80092C7C EndRace from FightUpdate's phase-3 bust"); }
    bool Arrest(uint32_t a, uint32_t v, int32_t k, uint32_t) override { // RASHCDG 0x80096F30, PORTED (race_modes.cpp)
        return s_->ModeArrest(a, v, static_cast<uint32_t>(k), 0);
    }
    bool PadMotor(int32_t port, int32_t a, int32_t b, int32_t c) override {
        if (rr::game::RumbleOn()) { // SLUS 0x8001DD74, PORTED (rumble_product.h)
            rr::game::ProductPadMotor(g_, static_cast<uint32_t>(port), a, b, c);
            return !g_.Faulted();
        }
        s_->NoteSeam("SLUS 0x8001DD74 the pad motors (HitRumble 0x800BFD74) are not driven: no rumble, nothing else");
        return true;
    }
    bool RiderSpeech(uint32_t h, int32_t a) override { // SLUS 0x8001A760, PORTED (speech_session.cpp)
        return s_->RiderSpeech(h, a, 0, &seams_);
    }
    // The weapon callees, PORTED (weapon_session.h); one that refuses names itself, and
    // FightCommand restores the arena as for any refusal.
    bool WeaponObject(uint32_t r, uint32_t side) override { return Weapon().Object(r, side); }
    bool ObjectRelease(uint32_t r, uint32_t slot, uint32_t z) override { return Weapon().Release(r, slot, z); }
    bool ObjectStop(uint32_t slot) override { return Weapon().Stop(slot); }
    bool ObjectSound(uint32_t e, int32_t a1, int32_t a2, int32_t a3, int32_t a4) override {
        return Weapon().Effect(e, a1, a2, a3, a4);
    }
    bool OverlayClip(uint32_t ev, uint32_t r, uint32_t a2, uint32_t a3, uint32_t& v0) override {
        return Weapon().Clip(ev, r, a2, a3, v0);
    }
    ProductWeapon Weapon() { return ProductWeapon(g_, seams_, s_); }

private:
    bool Refuse(const char* what) {
        refused = true;
        s_->NoteSeam(std::string(what) + " is not ported: a fight step that reaches it is REFUSED and the arena "
                                         "restored");
        return false;
    }
    RaceSession* s_;
    GuestRam& g_;
    uint8_t* ram_;
    rr::sim::StanceSeams& seams_;
    SoundRuntime& snd_;
};

// RASHCDI 0x800653E8: `DATA\FIGHT.BIN`, loaded whole, the two file offsets of each of the 40 records
// (+4 the edge block, +8 the node block) turned into pointers, the pointer published at 0x8005AD4C
// (rules.md 8.1). At OUR address: rr-race's 0x801A6B5C lies in this arena's cell buffers.
void RaceSession::LoadFightTable(const DiscImage& disc) {
    const auto f = disc.Find("DATA/FIGHT.BIN");
    if (!f) {
        NoteSeam("DATA/FIGHT.BIN is not on the disc: *(0x8005AD4C) stays 0 and there is no combat");
        return;
    }
    std::vector<uint8_t> file = disc.ReadFile(*f);
    if (file.size() < 12u * kFightRecords || file.size() > 0x4000u) {
        NoteSeam("DATA/FIGHT.BIN has an unexpected size: *(0x8005AD4C) stays 0 and there is no combat");
        return;
    }
    GuestRam g(arena_.Ram(), kGp);
    bool ported = false;
    if (Loader2On()) { // SetUpRace's FightLoad RASHCDI 0x800653E8 PORTED ("%s.bin" of "DATA\FIGHT", its 40
        // records' +4 / +8 made absolute), with the overlay laid (its format string is RASHCDI's); LoadFile answered with
        // the file laid at kArenaFight (OURS: where) - SetUpRace's site hands the pointer back at its point (race_modes.cpp)
        LoaderOverlay ov(g, disc);
        ProductLoaderCallees c(g, nullptr, &disc, nullptr, 0,
                               [&](uint32_t fn, const uint32_t* a, int, uint32_t, uint32_t& v0, bool& handled) {
                                   handled = fn == rr::sim::kLdLoadFile;
                                   if (!handled) return true;
                                   g.WriteBlock(kArenaFight, file.data(), static_cast<uint32_t>(file.size()));
                                   g.W32(a[2], kArenaFight);
                                   g.W32(a[3], static_cast<uint32_t>(file.size()));
                                   v0 = static_cast<uint32_t>(file.size());
                                   return !g.Faulted();
                               });
        uint32_t v0 = 0;
        if (ov.ok() && rr::sim::FightLoad(g, 0x800524DCu, kLoaderSp, c, v0) && !g.Faulted() && v0 == kArenaFight) {
            ported = true;
            ++Loader2Totals().fightLoad;
        }
        g.ClearFault();
    }
    if (!ported) {
        g.WriteBlock(kArenaFight, file.data(), static_cast<uint32_t>(file.size()));
        for (uint32_t i = 0; i < kFightRecords; ++i)
            for (uint32_t k : {4u, 8u}) g.W32(kArenaFight + 12u * i + k, g.U32(kArenaFight + 12u * i + k) + kArenaFight);
    }
    g.W32(F::kFightRecPtr, kArenaFight);
    // The pad record's control table (player 1's +0xB8, 0x800526A4 in every capture) - the combat
    // input map reaches the slots through it.
    g.W32(kPadRecord + 0xB8u, kPadRemap);
    NoteSeam(std::string("FIGHT.BIN is at 0x8000C000 (ours; rr-race has it at 0x801A6B5C, where this arena keeps cell buffers)") +
             (ported ? ", read and relocated by FightLoad RASHCDI 0x800653E8 (PORTED, loader2)" : ", relocated by the session (transcribed)"));
}

// The pad handler's combat tail for player 1, in the original's order: the post-processor's slot loop
// (SLUS 0x8001C8F4..0x8001C9FC, TRANSCRIBED, not benched) stamps the 15 button slots of the pad record
// from the repacked word, then SLUS 0x8001D344..0x8001D37C calls the PORTED CombatDecode 0x800C2348
// when the bike's top command is not 16 and the PORTED ComboInput 0x800C258C when it is.
void RaceSession::FightPadPass(const PadState& pad, rr::sim::StanceSeams& seams, int p) {
    GuestRam g(arena_.Ram(), kGp);
    const uint32_t gs = g.U32(kGameStatePtr);
    // The pause (pause_product.cpp): the frame's stamp may already be done - before the pad poll's
    // pause test, which reads the Start slot's press code, as the original's frame copy comes first.
    if (!padStamped_[p == 0 ? 0 : 1]) FightPadStamp(pad, p);
    padStamped_[p == 0 ? 0 : 1] = false;
    const uint32_t rec = kPadRecord + 192u * static_cast<uint32_t>(p);
    if (g.U32(F::kFightRecPtr) == 0) return;
    FightPadDecode(g, gs, rec, seams, p);
    if (p == 0 && pad.blow.pending) FightPhysicalBlow(pad.blow, seams); // VR physical combat
}

// The post-processor's slot loop for player `p`'s record (see FightPadPass).
void RaceSession::FightPadStamp(const PadState& pad, int p) {
    GuestRam g(arena_.Ram(), kGp);
    const uint32_t gs = g.U32(kGameStatePtr);
    padStamped_[p == 0 ? 0 : 1] = true;
    if (p == 0) { // once a frame; player 2's record copies the same clock
        // game_state +0x0C is the tick clock the pad record's +0x00 copies and +0x20 the frame's ticks
        // (equal to +0x18 / +0x1C in every capture): their writer is not ported, so both are ours.
        // The weapon in hand riderDef +0x2E is the rider-record loader's (RASHCDI 0x80064C0C: 9, fists, when
        // it resets the weapons; grid_loader.h).
        if (rr::game::PassesWhole()) {
            // race_session.cpp Frame: +0x0C is the frame clock (the VSync callback's, OURS) and
            // +0x20 is written by the PORTED FrameDelta SLUS 0x8001C428; the pad record copies the clock
            padClock_ = g.U32(gs + 0x0Cu);
        } else {
            padClock_ += static_cast<uint32_t>(g.S32(gs + 0x1Cu));
            g.W32(gs + 0x0Cu, padClock_);
            g.W32(gs + 0x20u, g.U32(gs + 0x1Cu));
            NoteSeam("RRJB_PASSES=session: game_state +0x0C (the pad clock) and +0x20 (the frame's ticks) are written "
                     "by this session");
        }
    }
    // The repacked word: bits 0..7 the second button byte, 8..15 the first, 1 = pressed.
    uint32_t word = 0;
    if (pad.taunt) word |= 1u << 0; // L2, slot 9: the taunt (speech_session.cpp TauntPad)
    if (pad.triangle) word |= 1u << 4; // slot 6: the pause menu's back
    if (pad.start) word |= 1u << 11;   // slot 12: control 1, the pause
    if (pad.r2) word |= 1u << 1;
    if (pad.l1) word |= 1u << 2;
    if (pad.r1) word |= 1u << 3;
    if (pad.throttle) word |= 1u << 6;
    if (pad.brake) word |= 1u << 7;
    if (pad.padUp) word |= 1u << 12;
    if (pad.right) word |= 1u << 13;
    if (pad.padDown) word |= 1u << 14;
    if (pad.left) word |= 1u << 15;
    const uint32_t rec = kPadRecord + 192u * static_cast<uint32_t>(p); // player p's record (the pad reader's loop)
    if (rr::game::PauseOn()) // the pause: last frame's press codes and negative stamps cleared here, before the
        for (uint32_t k = 0; k < 19; ++k) { // stamp (0x8001CBB4..0x8001CBD4 on the driver's record; pad_product.cpp)
            const uint32_t sl = rec + 0x14u + 8u * k;
            g.W8(sl + 6u, 0);
            if (g.S32(sl) < 0) g.W32(sl, 0);
        }
    g.W32(rec, padClock_);
    for (uint32_t i = 0; i < 15; ++i) {
        const uint32_t stampAt = rec + 0x14u + 8u * i;
        const uint32_t b = rec + 0x19u + 8u * i;         // +0x05: the frame counter, capped at 31
        const int32_t stamp = g.S32(stampAt);
        if ((word & g.U32(kSlotMasks + 4u * i)) == 0) {
            g.W8(b - 1u, 0);
            if (stamp > 0) g.W32(stampAt, static_cast<uint32_t>(stamp) - g.U32(rec));
        } else if (stamp < 1) {
            g.W32(stampAt, g.U32(rec));
            g.W8(b + 1u, g.U8(b) < 11u ? 2 : 1);
            g.W8(b, 0);
            g.W8(b + 2u, 0);
            g.W8(b - 1u, 1);
        } else {
            const uint8_t st = g.U8(b - 1u);
            if (static_cast<int8_t>(st) >= 0 && ((st == 0 && g.U8(b + 2u) > 2u) || (st != 0 && g.U8(b + 2u) > 30u))) {
                g.W8(b - 1u, 0);
                g.W8(b + 1u, 0xFF);
                g.W8(b + 2u, 0);
            }
            g.W8(b + 2u, static_cast<uint8_t>(g.U8(b + 2u) + 1u));
        }
        if (g.U8(b) < 31u) g.W8(b, static_cast<uint8_t>(g.U8(b) + 1u));
    }
}

// SLUS 0x8001D344..0x8001D37C, the combat tail (see FightPadPass).
void RaceSession::FightPadDecode(GuestRam& g, uint32_t gs, uint32_t rec, rr::sim::StanceSeams& seams, int p) {
    const uint32_t bike = bikes_[static_cast<size_t>(p)].entityAddress;
    // The combat tail's gate: SLUS 0x8001D350..0x8001D37C sits inside the pad reader's
    // ON-THE-BIKE block - reached only when game_state[0] == 1, the bike is under pad control (flagsA +0x230
    // bit 27 clear), the rider is active (+0x140 != 0) and still on the bike (+0x25C < 2: `sltiu` at
    // 0x8001D018, `beqz` to the walk block 0x8001D5B0). A knocked-off player gets no fight command; without
    // this gate a punch press re-entered a fight stance (92, mount 1) from the fall and the pending crash
    // request ran RiderKnockOff's body again. RRJB_PADGATE=off: the negative control.
    {
        const uint32_t rider = g.U32(bike + 0x354u);
        const bool onBike = g.U8(gs) == 1u && !(g.U32(bike + 0x230u) & 0x08000000u) &&
                            g.S16(rider + 0x140u) != 0 && g.U32(rider + 0x25Cu) < 2u;
        PadGateNoteFall(g.U32(rider + 0x25Cu), g.S32(gs + 0x10u), p);
        if (!onBike) {
            if (PadGateOn()) {
                ++(g.U32(rider + 0x25Cu) >= 2u ? PadGateCount().skipped : PadGateCount().skippedOther);
                return;
            }
            ++PadGateCount().ungated;
        }
    }
    const uint32_t top = bike + 0x3B4u + 8u * static_cast<uint32_t>(static_cast<int32_t>(g.S8(bike + 0x3B2u)));
    ProductFight c(this, g, arena_.Ram(), seams, sounds_);
    bool ok;
    if (g.U16(top) != 16) {
        int32_t v0 = 0;
        ok = F::CombatDecode(g, c, rec, bike, kFightSp, v0);
        if (ok && v0 != 0) ++log_.fight.decodes;
    } else {
        ok = F::ComboInput(g, rec, bike);
        ++log_.fight.combos;
    }
    if (!ok || g.Faulted()) {
        g.ClearFault();
        NoteSeam("RASHCDG 0x800C2348 / 0x800C258C (PORTED) refused on the player's pad record");
    }
}

bool RaceSession::FightCommand(uint32_t e, uint32_t target, rr::sim::StanceSeams& seams) {
    GuestRam g(arena_.Ram(), kGp);
    g.SetScratchpad(scratchpad_.data());
    SnapshotArena();
    ProductFight c(this, g, arena_.Ram(), seams, sounds_);
    std::vector<uint8_t> hp(bikes_.size());
    for (size_t i = 0; i < bikes_.size(); ++i) hp[i] = bikes_[i].riderDef[0x0F];
    if (e == bikes_[0].entityAddress && target != 224u) { // the log: the geometry FightUpdate is about to test
        const uint32_t t = g.U32(0x8005B3A0u) + 1096u * target;
        auto project = [&](uint32_t p, uint32_t axis, uint32_t q) {
            int32_t a[3], o[3];
            int16_t n[3];
            for (uint32_t k = 0; k < 3; ++k) {
                a[k] = g.S32(p + 4u * k);
                o[k] = g.S32(q + 4u * k);
                n[k] = g.S16(axis + 2u * k);
            }
            return rr::sim::AiProject(a, n, o);
        };
        log_.fight.along = project(e + 0x1F8u, t + 0x210u, t + 0x1F8u);
        const uint32_t mr = g.U32(e + 0x168u);                  // FightUpdate's lateral (0x800C04E0..0x800C054C)
        if (mr == g.U32(t + 0x168u) && ((mr >> 16) == 0 || g.U32(e + 0x150u) == g.U32(t + 0x150u)))
            log_.fight.lat = (g.S32(t + 0x16Cu) < 0 ? -1 : 1) * (g.S32(e + 0x158u) - g.S32(t + 0x158u));
        else
            log_.fight.lat = project(e + 0xB8u, t + 0x1B0u, t + 0xB8u);
        log_.fight.dy = g.S32(t + 0xBCu) - g.S32(e + 0xBCu);
        log_.fight.ownLat = (g.S32(e + 0x16Cu) < 0 ? -1 : 1) * (g.S32(e + 0x158u) - g.S32(t + 0x158u));
        log_.fight.height = g.S32(e + 0x138u);
        log_.fight.engage = F::CanEngage(g, e, t, -log_.fight.along);
    }
    const uint32_t rider = g.U32(e + 0x354u);
    const uint8_t firedBefore = g.U8(rider + 0x222u);
    const uint32_t victimBefore = g.U32(F::kLastBlow + 4u), timeBefore = g.U32(F::kLastBlow + 8u); // player 1's blow
    const uint8_t landedBefore = g.U8(F::kFightStats + 2u); // FightStat(player, 2, 0): the player's landed blows
    const bool ok = F::FightUpdate(g, c, e, static_cast<uint16_t>(target), kFightSp) && !g.Faulted();
    for (size_t i = 0; i < bikes_.size() && i < 2u && i < static_cast<size_t>(players_); ++i)
        if (ok && e == bikes_[i].entityAddress) PadGateSeen(g.U32(rider + 0x25Cu), static_cast<int>(i));
    if (ok && e == bikes_[0].entityAddress &&
        (g.U32(F::kLastBlow + 4u) != victimBefore || g.U32(F::kLastBlow + 8u) != timeBefore)) {
        ++log_.fight.strikes;
        ++FightTotalsCount().blows;
        if (g.U8(F::kFightStats + 2u) != landedBefore) {
            ++log_.fight.landed;
            ++FightTotalsCount().landed;
        }
    }
    if (ok && e == bikes_[0].entityAddress && firedBefore == 0 && g.U8(rider + 0x222u) != 0) ++log_.fight.strikeFrames;
    ++log_.fight.updates;
    if (!ok) {
        RestoreArena(g);
        ++log_.fight.refused;
        if (!c.refused) NoteSeam("RASHCDG 0x800C035C FightUpdate (PORTED) met an address this arena faults on: refused");
        return true; // the arm itself ran; the command pass carries on
    }
    for (size_t i = 0; i < bikes_.size(); ++i) {
        const uint8_t now = bikes_[i].riderDef[0x0F];
        if (now < hp[i]) {
            ++log_.fight.hits;
            NoteHurt(i, now);
            if (now == 0) ++log_.fight.knockOffs;
            log_.fight.note += " b" + std::to_string(i) + " hp " + std::to_string(hp[i]) + "->" + std::to_string(now);
        }
    }
    return true;
}

// Op 9's strike (strike_product.h): the PORTED Strike RASHCDG 0x800C1370 on the same
// product callees as FightUpdate; a refusal restores the arena as FightCommand's does.
bool RaceSession::StrikeCommand(uint32_t e, uint32_t t, rr::sim::StanceSeams& seams) {
    GuestRam g(arena_.Ram(), kGp);
    g.SetScratchpad(scratchpad_.data());
    SnapshotArena();
    ProductFight c(this, g, arena_.Ram(), seams, sounds_);
    std::vector<uint8_t> hp(bikes_.size());
    for (size_t i = 0; i < bikes_.size(); ++i) hp[i] = bikes_[i].riderDef[0x0F];
    F::StrikeTrace tr;
    const bool ok = F::Strike(g, c, e, t, tr) && !g.Faulted();
    StrikeCounts& n = StrikeCounters();
    ++n.calls;
    if (!ok) {
        RestoreArena(g);
        ++n.refused;
        if (!c.refused) NoteSeam("RASHCDG 0x800C1370 Strike (PORTED) met an address this arena faults on: refused");
        return true; // the arm itself ran; the command pass carries on
    }
    n.stepped += tr.stepped;
    n.restarted += tr.restarted;
    n.armed += tr.armed;
    n.hits += tr.hit;
    n.leans += tr.lean;
    for (size_t i = 0; i < bikes_.size(); ++i) {
        const uint8_t now = bikes_[i].riderDef[0x0F];
        if (now < hp[i]) {
            ++n.hpDrops;
            NoteHurt(i, now);
            if (now == 0) ++n.knockOffs;
        }
    }
    return true;
}

// ---- VR physical combat (fight_physical.h): a contact the VR layer measured (tools\rrgame\vr_melee.h),
// applied after the pad's own decode, behind the same gate as the decode (the pad reader's on-the-bike block), on the
// PORTED fight code with the product's callees; a refusal restores the arena as FightCommand's does.
namespace {
struct PhysicalCounts {
    uint64_t requests = 0, applied = 0, landed = 0, skipped = 0, refused = 0, stolen = 0, hurt = 0, knockOffs = 0;
    uint64_t pushed = 0, retargeted = 0, viaEdge = 0, swings = 0, matched = 0;
    uint64_t snatches = 0, snatchStolen = 0, snatchWaits = 0, snatchMissed = 0, snatchSkipped = 0; // weapon snatches
    std::string lastSkip, lastSnatch;
};
PhysicalCounts& PhysicalCount() {
    static PhysicalCounts c;
    return c;
}
SnatchResult g_lastSnatch; // the last grab's outcome (fight_session.h)
void NoteSnatch(uint32_t frame, uint32_t victim, SnatchOutcome o, int weapon) {
    g_lastSnatch.frame = frame;
    g_lastSnatch.victim = victim;
    g_lastSnatch.outcome = o;
    g_lastSnatch.weapon = weapon;
    ++g_lastSnatch.serial;
}
} // namespace

const SnatchResult& LastSnatch() { return g_lastSnatch; }

std::string PhysicalBlowTotals() {
    const PhysicalCounts& c = PhysicalCount();
    std::string s = "the physical blows: " + std::to_string(c.requests) + " contact(s) sent, " +
                    std::to_string(c.applied) + " applied through ApplyHit 0x800C17B0 (landed " + std::to_string(c.landed) +
                    ", riders hurt " + std::to_string(c.hurt) + ", the drop equal to the original's formula " +
                    std::to_string(c.matched) + ", knocked to 0 " + std::to_string(c.knockOffs) + "), a weapon stolen " +
                    std::to_string(c.stolen) + ", skipped " + std::to_string(c.skipped) + ", refused " +
                    std::to_string(c.refused) + "; the fight command pushed " + std::to_string(c.pushed) + ", retargeted " +
                    std::to_string(c.retargeted) + ", the strike node by its combo edge " + std::to_string(c.viaEdge) +
                    ", swings spent " + std::to_string(c.swings);
    if (!c.lastSkip.empty()) s += "; last skipped: " + c.lastSkip;
    s += "\nthe weapon snatches: " + std::to_string(c.snatches) + " grab(s) sent, the weapon taken through "
         "WeaponSteal 0x800BFF04 " + std::to_string(c.snatchStolen) + ", held for the rival's steal window " +
         std::to_string(c.snatchWaits) + ", punch started but WeaponSteal declined " + std::to_string(c.snatchMissed) +
         ", skipped " + std::to_string(c.snatchSkipped);
    if (!c.lastSnatch.empty()) s += "; last: " + c.lastSnatch;
    return s + "\n";
}

void RaceSession::FightPhysicalBlow(const PhysicalBlowRequest& req, rr::sim::StanceSeams& seams) {
    PhysicalCounts& n = PhysicalCount();
    ++n.requests;
    GuestRam g(arena_.Ram(), kGp);
    g.SetScratchpad(scratchpad_.data());
    const uint32_t gs = g.U32(kGameStatePtr);
    const uint32_t bike = bikes_[0].entityAddress;
    const uint32_t rider = g.U32(bike + 0x354u);
    const auto skip = [&](const std::string& why) {
        ++n.skipped;
        n.lastSkip = "f" + std::to_string(log_.frame) + " " + why;
        std::printf("physical blow: f%u skipped - %s\n", log_.frame, why.c_str());
    };
    // the pad reader's on-the-bike block (FightPadDecode's gate)
    if (!(g.U8(gs) == 1u && !(g.U32(bike + 0x230u) & 0x08000000u) && g.S16(rider + 0x140u) != 0 &&
          g.U32(rider + 0x25Cu) < 2u)) {
        skip("the player is not riding under pad control");
        if (req.snatch) NoteSnatch(log_.frame, req.victim, SnatchOutcome::kSkipped, 9);
        return;
    }
    size_t victim = 0;
    for (size_t i = 1; i < bikes_.size(); ++i)
        if (bikes_[i].entityAddress == req.victim) victim = i;
    SnapshotArena();
    ProductFight c(this, g, arena_.Ram(), seams, sounds_);
    std::vector<uint8_t> hp(bikes_.size());
    for (size_t i = 0; i < bikes_.size(); ++i) hp[i] = bikes_[i].riderDef[0x0F];
    const uint8_t landedBefore = g.U8(F::kFightStats + 2u); // FightStat(player, 2, 0)
    PhysicalBlowTrace tr;
    if (req.snatch) { // the grab, through WeaponSteal's path (fight_physical.h PhysicalSnatchRun)
        ++n.snatches;
        --n.requests;
        const uint32_t pd = g.U32(bike + 0x43Cu), vd = g.U32(req.victim + 0x43Cu);
        const unsigned mineBefore = g.U8(pd + 0x2Eu), maskBefore = g.U16(pd + 0x2Cu) & 0x1FFu;
        const unsigned hisBefore = g.U8(vd + 0x2Eu), hisMask = g.U16(vd + 0x2Cu) & 0x1FFu;
        const bool sok = PhysicalSnatchRun(g, c, seams, bike, req.victim, kFightSp, tr) && !g.Faulted();
        if (!sok) {
            RestoreArena(g);
            ++n.refused;
            NoteSnatch(log_.frame, req.victim, SnatchOutcome::kRefused, static_cast<int>(hisBefore));
            std::printf("weapon snatch: f%u REFUSED (the arena restored)\n", log_.frame);
            return;
        }
        char b[640];
        if (tr.skipped != nullptr) {
            ++(tr.wait ? n.snatchWaits : n.snatchSkipped);
            NoteSnatch(log_.frame, req.victim, tr.wait ? SnatchOutcome::kWait : SnatchOutcome::kSkipped, static_cast<int>(hisBefore));
            std::snprintf(b, sizeof(b), "f%u %s hand on rival b%zu's weapon %u: %s (his clip frame %d of %d)", log_.frame,
                          req.right ? "right" : "left", victim, hisBefore, tr.skipped, tr.victimFrame, tr.victimLast);
            n.lastSnatch = b;
            std::printf("weapon snatch: %s\n", b);
            return;
        }
        const uint32_t vRider = g.U32(req.victim + 0x354u);
        std::snprintf(b, sizeof(b),
                      "f%u the %s hand on rival b%zu's weapon %u (his clip frame %d of %d): command %u, FIGHT record %u "
                      "node %u, the player's clip %d -> %d (hitFrame %d; StealWindow %d at %d of %d)%s -> %s; the player's "
                      "weapon %u -> %u (owned %03X -> %03X), rival b%zu's %u -> %u (owned %03X -> %03X), his stance %u",
                      log_.frame, req.right ? "right" : "left", victim, hisBefore, tr.victimFrame, tr.victimLast, tr.cmd,
                      tr.record, tr.node, tr.frameBefore, tr.frameAfter, tr.hitFrame, tr.myWindow, tr.myFrame, tr.myLast,
                      tr.pushed ? ", fight pushed" : "", tr.stole ? "STOLEN by WeaponSteal" : tr.parried ? "WeaponSteal parried" : "WeaponSteal declined",
                      mineBefore, g.U8(pd + 0x2Eu), maskBefore, g.U16(pd + 0x2Cu) & 0x1FFu, victim, hisBefore, g.U8(vd + 0x2Eu),
                      hisMask, g.U16(vd + 0x2Cu) & 0x1FFu, g.U16(vRider + 0x220u));
        n.lastSnatch = b;
        std::printf("weapon snatch: %s\n", b);
        n.pushed += tr.pushed;
        ++(tr.stole ? n.snatchStolen : n.snatchMissed);
        NoteSnatch(log_.frame, req.victim, tr.stole ? SnatchOutcome::kStolen : SnatchOutcome::kDeclined, static_cast<int>(hisBefore));
        if (tr.stole) ++n.stolen;
        return;
    }
    const bool ok = PhysicalBlowRun(g, c, seams, bike, req.victim, req.right, req.weapon, req.strength, kFightSp, tr) &&
                    !g.Faulted();
    if (!ok) {
        RestoreArena(g);
        ++n.refused;
        if (!c.refused) NoteSeam("VR physical blow: the PORTED fight code met an address this arena faults on: refused");
        std::printf("physical blow: f%u REFUSED (the arena restored)\n", log_.frame);
        return;
    }
    if (tr.skipped != nullptr) {
        skip(tr.skipped);
        return;
    }
    n.pushed += tr.pushed;
    n.retargeted += tr.retargeted;
    n.viaEdge += tr.viaEdge;
    n.swings += tr.swingSpent;
    if (tr.stole) {
        ++n.stolen;
    } else {
        ++n.applied;
        ++log_.fight.strikes;
        ++FightTotalsCount().blows;
        if (g.U8(F::kFightStats + 2u) != landedBefore) {
            ++n.landed;
            ++log_.fight.landed;
            ++FightTotalsCount().landed;
        }
    }
    const uint32_t victimRider = g.U32(req.victim + 0x354u);
    const unsigned victimStance = g.U16(victimRider + 0x220u), victimMount = g.U32(victimRider + 0x25Cu);
    std::string drop;
    for (size_t i = 0; i < bikes_.size(); ++i) {
        const uint8_t now = bikes_[i].riderDef[0x0F];
        if (now < hp[i]) {
            ++log_.fight.hits;
            NoteHurt(i, now);
            ++n.hurt;
            if (now == 0) {
                ++log_.fight.knockOffs;
                ++n.knockOffs;
            }
            log_.fight.note += " b" + std::to_string(i) + " hp " + std::to_string(hp[i]) + "->" + std::to_string(now);
            drop += " b" + std::to_string(i) + " " + std::to_string(hp[i]) + "->" + std::to_string(now);
            if (i == victim && static_cast<int>(hp[i]) - static_cast<int>(now) == std::min<int>(hp[i], tr.expected))
                ++n.matched;
        }
    }
    std::printf("physical blow: f%u the %s %s on rival b%zu (part %d, %.2f m/s): command %u, FIGHT record %u node %u "
                "(stance %u -> the rider's %u, clip frame %d -> %d of hitFrame %d, damage %u x%.2f = %u), side 0x%X%s%s%s "
                "-> %s; the original's formula %d; the rival's stance %u (the node's hit stance %u), mount %u; health%s\n",
                log_.frame, req.right ? "right" : "left", req.weapon ? "weapon" : "fist", victim, req.part,
                double(req.speed), tr.cmd, tr.record, tr.node, tr.stance, tr.stanceAfter, tr.frameBefore, tr.frameAfter,
                tr.hitFrame, tr.damage,
                double(req.strength), tr.used, tr.side, tr.pushed ? ", fight pushed" : "",
                tr.retargeted ? ", retargeted" : "", tr.swingSpent ? ", a swing spent" : "",
                tr.stole ? "the weapon STOLEN" : "ApplyHit", tr.expected, victimStance, unsigned(tr.hitStance), victimMount,
                drop.empty() ? " unchanged" : drop.c_str());
}

void RaceSession::DevOpponentHealth(uint8_t hp) {
    for (size_t i = 1; i < bikes_.size(); ++i) {
        bikes_[i].riderDef[0x0E] = hp; // the regeneration ceiling AiPlan 0x800B8284 heals +0x0F up to
        bikes_[i].riderDef[0x0F] = hp;
    }
    NoteSeam("DEVELOPMENT: every opponent's health riderDef+0x0F and its regeneration ceiling +0x0E were set to " +
             std::to_string(hp) + " (rrgame --opponent-health)");
}

uint32_t RaceSession::FightCombatLeave(uint32_t r, uint32_t cur, uint32_t ev, uint32_t p, rr::sim::StanceSeams& seams) {
    GuestRam g(arena_.Ram(), kGp);
    ProductFight c(this, g, arena_.Ram(), seams, sounds_);
    uint32_t v0 = 3;
    ++log_.fight.leaves;
    if (!F::CombatLeave(g, c, r, cur, ev, p, v0) || g.Faulted()) {
        g.ClearFault();
        NoteSeam("RASHCDG 0x800BFD24 (PORTED) refused: the stance change carries on as if it returned");
    }
    return v0;
}

uint32_t RaceSession::FightCombatEnter(uint32_t r, uint32_t cur, uint32_t ev, uint32_t p, rr::sim::StanceSeams& seams) {
    GuestRam g(arena_.Ram(), kGp);
    ProductFight c(this, g, arena_.Ram(), seams, sounds_);
    uint32_t v0 = 0;
    ++log_.fight.enters;
    if (!F::CombatEnter(g, c, r, cur, ev, p, v0) || g.Faulted()) {
        g.ClearFault();
        NoteSeam("RASHCDG 0x800BFC5C (PORTED) refused: the stance change carries on as if it returned");
    }
    return v0;
}

} // namespace rr::game
