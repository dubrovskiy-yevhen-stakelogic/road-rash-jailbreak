#pragma once
// VR physical combat (tools\rrgame\vr_melee.h) - OURS: the original has the pad only. In VR a blow is a
// CONTACT - the player's tracked fist or the weapon in the hand touched a rival rider's posed body - and it is applied
// on THAT rider through the ORIGINAL's fight system, so damage, the victim's reaction stance, the knock-off, the hit
// sound, the HUD bar, NoteHit / the grudges and the AI's answer are the original's. Only ReachTest's GEOMETRY and the
// animation's strike-frame timing are replaced (the contact is the reach and the moment); every function below is a
// PORTED one of src\game\sim\fight.h, run on the product's callees (fight_session.cpp FightPhysicalBlow).
//
// The sequence, each step the state the pad path leaves at the moment a blow lands:
//   1. the command CombatDecode RASHCDG 0x800C2348 would issue for this hand and the weapon in hand (riderDef +0x2E):
//      fists 32 (right, R1) / 36 (left, L1); a weapon 0..5 142 / 146; 6 / 7 / 8 148 (their L1+Up / R1+Up swing) -
//      written to riderDef +0x3C (0x800C2534), rider +0x23D := 0 (0x800C2568);
//   2. the fight command {16, the victim} on the player's own stack as FightPush 0x800C1DD4 pushes it (AiPushCommand
//      mode 2), unless the top already is it; a top 16 on ANOTHER rider is retargeted in place (OURS: the pad path
//      never meets that - its decode does not run while the top is 16 - and a second 16 under it would keep a fight
//      with the old rider alive after FightEnd pops the first);
//   3. FightStart / FightBegin's writes (0x800C110C / 0x800C12D8: rider +0x23C bits 0 / 1, the record and queue of
//      FightPickRecord 0x800BF978, +0x230 / +0x234 / +0x238), then the queue as ComboInput 0x800C258C leaves it when
//      the record's first slot-1 edge from the wind-up queues its striking node (a node with damage; +0x23E[1],
//      +0x23A = 1, +0x230 / +0x234 / +0x238 / +0x23D as its `accept`); a record with no such edge strikes with its
//      first damaging node. The strike node's stance through the stance event (FightBegin's `side | 0x10`), and rider
//      +0x222 := 1 - the strike has fired (FightStep's 0x800C0A8C), so the op-16 arm does not fire it a second time;
//      the rider's animation clip is then run forward (the PORTED AdvanceFrame 0x8005CB04, one frame's dt a call, as the
//      animation pass steps it) to the node's hitFrame - OURS: the contact IS the strike moment, so the clip, and its
//      noise list (AnimSounds: the swing and the hit sound), stand where the pad path's ApplyHit finds them;
//   4. FightUpdate's tail on this victim with the contact for ReachTest: the side bit from the road lateral as
//      0x800C04E0..0x800C054C computes it; an armed start spends a swing as 0x800C0770 does; command 32 runs
//      WeaponSteal 0x800BFF04 (a steal blocks the blow, as ReachTest's `flags & 0x80`); then ApplyHit 0x800C17B0 with
//      the hit bit set - the node's damage x `strength` (OURS, the "strength from swing speed" setting; 1 = the
//      original's) laid in the node for that one call and put back.
#include <cstdint>

#include "game/sim/anim.h"
#include "game/sim/fight.h"

namespace rr::game {

struct PhysicalBlowTrace {
    const char* skipped = nullptr; // a state the pad path would not strike from: nothing written
    uint8_t cmd = 0, record = 0, node = 0;
    uint16_t stance = 0, stanceAfter = 0; // the strike node's stance, the attacker's stance after the event
    uint16_t hitStance = 0;               // the node's stance for a hit victim (HitStance 0x800BF860's +4)
    uint16_t damage = 0, used = 0;        // the node's base damage, what ApplyHit read (strength applied)
    uint32_t side = 0;                    // FightUpdate's side bit (0x100 = the victim on the positive lateral)
    int expected = 0;                     // ApplyHit's `final` by its formula, for the log
    int hitFrame = 0, frameBefore = 0, frameAfter = 0; // the strike node's hitFrame, the clip's frame before / after
    bool pushed = false, retargeted = false, entered = false, viaEdge = false, swingSpent = false;
    bool stole = false, applied = false;
    // the snatch (PhysicalSnatchRun): the victim's clip frame / its last frame when the grab came, the
    // player's clip frame / last after the run forward, WeaponSteal's own window test on the player (StealWindow)
    bool snatch = false, wait = false, missed = false, parried = false;
    int victimFrame = -1, victimLast = -1, myFrame = -1, myLast = -1, myWindow = -1;
    uint8_t weaponTaken = 9;
};

// The blow of the player's bike `me` on `victim`; `sp` the fight code's stack. False: a callee refused or the view
// faulted (the caller restores the arena). True with `skipped` set: nothing was done.
// `pose`: the animation machine's pose side (the product's stance seams) for the clip's run forward.
bool PhysicalBlowRun(rr::sim::GuestRam& g, rr::sim::fight::Callees& c, rr::sim::AnimPoseSeam& pose, uint32_t me,
                     uint32_t victim, bool right, bool weapon, float strength, uint32_t sp, PhysicalBlowTrace& tr);

// VR weapon snatching (tools\rrgame\vr_snatch.h) - OURS: the player's free hand closed its grip on the
// weapon a rival is swinging at him. In the original a weapon changes hands ONLY through WeaponSteal RASHCDG 0x800BFF04
// (rules.md 8.3: "stolen, never picked up"), reached from FightUpdate when the thief's command is the bare-fisted
// punch 32 (0x800C0808), its fight record 0 node 1 (StealWindow 0x800C2E9C) and the victim mid-swing with a weapon
// (stance category 3, command bit 0x80) past half of his clip and not past three quarters of it. The grab takes that
// path and nothing else: the victim's state is read first (no write) - not swinging a weapon, or past 3/4 of his clip:
// nothing; short of 1/2: `wait` (the VR layer holds the grab and asks again next frame); in the window: steps 1..3 of
// PhysicalBlowRun with the right-punch command 32 (the only command the original steals with), the player's clip run
// forward to its hitFrame (and on into StealWindow's own 1/4..3/4 when the hitFrame is short of it), then the PORTED
// WeaponSteal - ownership, the swing counters, the HUD icon, the objects, the victim's reaction stance, the steal
// sound are the original's. No ApplyHit: a grab is not a blow (the strike is marked fired, +0x222, as for a blow).
// If WeaponSteal does not take it (its own test), the punch stands as a pad punch would - the ported FightUpdate on
// the player keeps offering WeaponSteal the next frames, exactly as the original does.
bool PhysicalSnatchRun(rr::sim::GuestRam& g, rr::sim::fight::Callees& c, rr::sim::AnimPoseSeam& pose, uint32_t me,
                       uint32_t victim, uint32_t sp, PhysicalBlowTrace& tr);

} // namespace rr::game
