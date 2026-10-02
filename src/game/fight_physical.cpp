// VR physical combat: a measured contact applied through the PORTED fight code (fight_physical.h).
#include "game/fight_physical.h"

#include <algorithm>
#include <cmath>

#include "game/sim/ai.h"

namespace rr::game {
namespace {

using rr::sim::GuestRam;
namespace F = rr::sim::fight;

constexpr uint32_t kRider = 0x354, kDef = 0x43C, kDepth = 0x3B2, kStack = 0x3B4;

uint32_t Category(GuestRam& g, uint32_t stance) { return g.U16(F::kStanceTab + 8u * (stance & 0xFFFFu) + 2u); }
uint32_t Rec(GuestRam& g, uint32_t i) { return g.U32(F::kFightRecPtr) + 12u * i; }
uint32_t NodeAt(GuestRam& g, uint32_t rec, uint32_t n) { return g.U32(Rec(g, rec) + 8u) + 12u * n; }
int32_t Project(GuestRam& g, uint32_t p, uint32_t axis, uint32_t q) {
    int32_t a[3], o[3];
    int16_t n[3];
    for (uint32_t k = 0; k < 3; ++k) {
        a[k] = g.S32(p + 4u * k);
        o[k] = g.S32(q + 4u * k);
        n[k] = g.S16(axis + 2u * k);
    }
    return rr::sim::AiProject(a, n, o); // RASHCDG 0x800B6AAC, ported (ai.h)
}

// CombatDecode 0x800C2348's command for action 1 (right) / 2 (left) and the weapon in hand, the weapons whose actions
// 1 / 2 are only a taunt taking their own swing (6 / 7: action 6, 8: action 5 - both 148); 0 for none.
uint8_t BlowCommand(uint32_t w, bool right, bool weapon) {
    if (!weapon || w == 9) return right ? 32 : 36;
    if (w <= 5) return right ? 142 : 146;
    if (w <= 8) return 148;
    return 0;
}

bool Skip(PhysicalBlowTrace& tr, const char* why) {
    tr.skipped = why;
    return true;
}

// A rider's current clip: its frame and its last frame (WeaponSteal 0x800BFF04's / StealWindow 0x800C2E9C's reads).
void ClipFrame(GuestRam& g, uint32_t r, int32_t& f, int32_t& n) {
    const uint32_t a = g.U32(r + 0x21Cu);
    const uint32_t b = g.U8(g.U32(a + 4u) + 12u * g.U32(a + 12u));
    const uint32_t clip = g.U32(g.U32(g.U32(a + 40u) + 4u) + 4u * b);
    f = g.S32(a + 16u);
    n = static_cast<int16_t>(static_cast<uint16_t>(g.U16(clip + 16u) - 1u));
}

bool Run(GuestRam& g, F::Callees& c, rr::sim::AnimPoseSeam& pose, uint32_t me, uint32_t t, bool right, bool weapon,
         float strength, uint32_t sp, PhysicalBlowTrace& tr, bool snatch);

} // namespace

bool PhysicalBlowRun(GuestRam& g, F::Callees& c, rr::sim::AnimPoseSeam& pose, uint32_t me, uint32_t t, bool right,
                     bool weapon, float strength, uint32_t sp, PhysicalBlowTrace& tr) {
    return Run(g, c, pose, me, t, right, weapon, strength, sp, tr, false);
}

bool PhysicalSnatchRun(GuestRam& g, F::Callees& c, rr::sim::AnimPoseSeam& pose, uint32_t me, uint32_t t, uint32_t sp,
                       PhysicalBlowTrace& tr) {
    return Run(g, c, pose, me, t, true, false, 1.0f, sp, tr, true);
}

namespace {

bool Run(GuestRam& g, F::Callees& c, rr::sim::AnimPoseSeam& pose, uint32_t me, uint32_t t, bool right, bool weapon,
         float strength, uint32_t sp, PhysicalBlowTrace& tr, bool snatch) {
    tr = PhysicalBlowTrace{};
    tr.snatch = snatch;
    const uint32_t r = g.U32(me + kRider);
    const uint32_t rd = g.U32(me + kDef);
    const uint32_t pool0 = g.U32(F::kPool0Ptr);
    if (g.U32(F::kFightRecPtr) == 0) return Skip(tr, "no FIGHT.BIN");
    if (t == me || t < pool0 || (t - pool0) % 1096u != 0 || (t - pool0) / 1096u >= 32u)
        return Skip(tr, "the target is not a pool-0 bike");
    const uint16_t slot = static_cast<uint16_t>((t - pool0) / 1096u);
    // the victim as CanEngage 0x800BC1EC takes one: live, its rider on the bike (+0x25C < 2)
    if (g.S16(t + 0x140u) == 0 || g.U32(g.U32(t + kRider) + 0x25Cu) >= 2u)
        return Skip(tr, "the rider touched is not live on his bike");
    // FightPush's own refusals (0x800C1DF4..): a finished race, a result code
    if (g.S32(rd + 40u) != 0 || !(g.U8(rd + 39u) < 248u)) return Skip(tr, "the player's race is over");
    // FightUpdate's lock arm (0x800C0430): stances 30..37 end the fight
    if ((g.U16(r + 0x220u) - 30u) < 8u) return Skip(tr, "the player's fight is ending (stance 30..37)");
    const uint32_t w = g.U8(rd + 46u);
    const uint8_t cmd = BlowCommand(w, right, weapon);
    if (cmd == 0) return Skip(tr, "no blow for the weapon in hand");
    tr.cmd = cmd;
    if (snatch) { // the victim's swing and WeaponSteal's window on it, read before anything is written
        const uint32_t vr = g.U32(t + kRider), vrd = g.U32(t + kDef);
        if (Category(g, g.U16(vr + 0x220u)) != 3) return Skip(tr, "the rival is not swinging (stance category not 3)");
        if (!(g.U8(vrd + 60u) & 0x80u) || g.U8(vrd + 46u) >= 9u) return Skip(tr, "the rival swings no weapon");
        if (g.U8(r + 0x23Bu) != 0xFFu) return Skip(tr, "the player's hand holds an object (mid-swing)");
        int32_t f = 0, n = 0;
        ClipFrame(g, vr, f, n);
        tr.victimFrame = f, tr.victimLast = n;
        tr.weaponTaken = g.U8(vrd + 46u);
        if (static_cast<int64_t>(n) * 3 < static_cast<int64_t>(f) * 4)
            return Skip(tr, "too late: the rival's swing is past 3/4 of its clip (WeaponSteal would only parry)");
        if (!(static_cast<int64_t>(n) < static_cast<int64_t>(f) * 2)) {
            tr.wait = true;
            return Skip(tr, "early: the rival's swing is short of half its clip (WeaponSteal's window) - held");
        }
    }

    // 1. the command (CombatDecode 0x800C2534)
    g.W8(rd + 60u, cmd);
    // 2. {16, victim} on the player's stack (FightPush 0x800C1FE8..0x800C2018)
    const uint32_t top = me + kStack + 8u * static_cast<uint32_t>(static_cast<int32_t>(g.S8(me + kDepth)) - 1);
    if (g.S8(me + kDepth) > 0 && g.U16(top) == 16) {
        if (g.U16(top + 2u) != slot) {
            g.W16(top + 2u, slot); // OURS: the retarget (fight_physical.h)
            tr.retargeted = true;
        }
    } else {
        const uint32_t at = sp - 40u - 32u + 16u; // FightPush's record in its frame, called from CombatDecode's
        uint8_t rec[8];
        g.ReadBlock(at, rec, 8);
        rec[0] = 16;
        rec[1] = 0;
        rec[2] = static_cast<uint8_t>(slot);
        rec[3] = static_cast<uint8_t>(slot >> 8);
        if (!c.PushCommand(rec, at, 2, me)) return false;
        tr.pushed = true;
    }
    g.W8(r + 0x23Du, 0); // CombatDecode 0x800C2568
    if (g.Faulted()) return false;

    // 3. the blow's record and its striking node
    tr.entered = Category(g, g.U16(r + 0x220u)) != 3;
    if (tr.entered) g.W8(r + 0x23Cu, static_cast<uint8_t>(g.U8(r + 0x23Cu) & 0xFEu)); // FightStart 0x800C1124
    g.W8(r + 0x23Cu, static_cast<uint8_t>(g.U8(r + 0x23Cu) & 0xFDu));                 // FightBegin 0x800C12F0
    const uint32_t record = F::FightPickRecord(g, me);
    g.W32(r + 0x230u, 0);
    g.W32(r + 0x234u, 0);
    g.W8(r + 0x238u, 0);
    tr.record = static_cast<uint8_t>(record);
    const uint32_t recAt = Rec(g, record);
    const uint32_t nodes = g.U8(recAt + 3u), edges = g.U8(recAt + 2u), edgeAt = g.U32(recAt + 4u);
    const uint32_t q0 = g.U8(r + 0x23Eu);
    int node = -1;
    for (uint32_t i = 0; i < edges && node < 0; ++i) { // ComboInput's slot-1 edge from the wind-up
        const uint32_t e = edgeAt + 12u * i;
        const uint32_t pk = g.U32(e + 8u);
        const uint32_t n = (pk >> 20) & 0x3Fu;
        if (((pk >> 8) & 0x3Fu) != 1u || ((pk >> 14) & 0x3Fu) != q0 || n >= nodes || g.U16(NodeAt(g, record, n) + 8u) == 0)
            continue;
        node = static_cast<int>(n);
        g.W8(r + 0x23Eu + 1u, static_cast<uint8_t>(n)); // ComboInput's `accept` (0x800C27A4..)
        g.W32(r + 0x230u, 0);
        g.W8(r + 0x238u, 1);
        g.W32(r + 0x234u, g.U32(e + 4u));
        if (i < 8u) g.W8(r + 0x23Du, static_cast<uint8_t>(g.U8(r + 0x23Du) | (1u << i)));
        g.W8(r + 0x23Au, 1);
        tr.viaEdge = true;
    }
    for (uint32_t n = 0; n < nodes && node < 0; ++n) // no such edge: the record's first damaging node
        if (g.U16(NodeAt(g, record, n) + 8u) != 0) {
            node = static_cast<int>(n);
            g.W8(r + 0x23Eu, static_cast<uint8_t>(n));
            g.W8(r + 0x23Au, 0);
        }
    if (node < 0) return Skip(tr, "the blow's FIGHT.BIN record has no damaging node") && !g.Faulted();
    tr.node = static_cast<uint8_t>(node);
    const uint32_t nd = NodeAt(g, record, static_cast<uint32_t>(node));
    tr.stance = g.U16(nd);
    tr.damage = g.U16(nd + 8u);
    tr.hitStance = g.U16(nd + 4u);

    // 4. the side (FightUpdate 0x800C04E0..0x800C054C, 0x800C07D4)
    int32_t lat;
    const uint32_t mr = g.U32(me + 0x168u);
    if (mr == g.U32(t + 0x168u) && ((mr >> 16) == 0 || g.U32(me + 0x150u) == g.U32(t + 0x150u))) {
        lat = static_cast<int32_t>(g.U32(me + 344u) - g.U32(t + 344u));
        if (g.S32(t + 364u) < 0) lat = -lat;
    } else {
        lat = Project(g, me + 184u, t + 432u, t + 184u);
    }
    const uint32_t side = (lat > 0 ? 1u : 0u) << 8;
    tr.side = side;
    uint32_t ignored = 0;
    if (!c.StanceEvent(tr.stance, r, side | 0x10u, ignored)) return false; // FightBegin's event (0x800C1358)
    g.W8(r + 0x222u, 1);                                                   // the strike fired (FightStep 0x800C0A8C)
    tr.stanceAfter = g.U16(r + 0x220u);
    {   // the clip run forward to the strike frame (fight_physical.h): AdvanceFrame 0x8005CB04 with the frame's dt, as the
        // pass 0x8005E1D8 calls it, while the clip is short of hitFrame (FightStep's own compare) and not done
        const uint32_t anim = g.U32(r + 0x21Cu);
        const int32_t dt = g.S32(g.U32(F::kGameStatePtr) + 28u);
        tr.hitFrame = g.U8(nd + 11u);
        tr.frameBefore = tr.frameAfter = g.S32(anim + rr::sim::animf::kFrame);
        rr::sim::AnimMachine m(g, pose);
        for (int guard = 0; guard < 64 && dt > 0; ++guard) {
            if (g.S32(anim + rr::sim::animf::kFrame) >= tr.hitFrame) break;
            if (!(g.U32(anim + rr::sim::animf::kFlags) & 2u) || (g.U32(anim + rr::sim::animf::kFlags) & 8u)) break;
            if (m.ClipDone(anim) != 0) break;
            m.AdvanceFrame(anim, dt);
            if (m.Failed()) return false;
        }
        tr.frameAfter = g.S32(anim + rr::sim::animf::kFrame);
        if (snatch) { // on into StealWindow's own 1/4 .. 3/4 of the punch when its hitFrame is short of it
            int32_t f = 0, n = 0;
            ClipFrame(g, r, f, n);
            for (int guard = 0; guard < 64 && dt > 0 && !(n < f * 4); ++guard) {
                if (!(g.U32(anim + rr::sim::animf::kFlags) & 2u) || (g.U32(anim + rr::sim::animf::kFlags) & 8u)) break;
                if (m.ClipDone(anim) != 0) break;
                m.AdvanceFrame(anim, dt);
                if (m.Failed()) return false;
                ClipFrame(g, r, f, n);
            }
            tr.frameAfter = g.S32(anim + rr::sim::animf::kFrame);
            tr.myFrame = f, tr.myLast = n;
            tr.myWindow = F::StealWindow(g, r, g.U8(g.U32(t + kDef) + 46u));
        }
    }
    // an armed start spends a swing (FightUpdate 0x800C0770..0x800C07FC)
    if (tr.entered && g.U8(rd + 47u) != 0 && (g.U8(rd + 60u) & 0x80u) && g.S8(r + 0x23Bu) != -1) {
        g.W8(rd + 47u, static_cast<uint8_t>(g.U8(rd + 47u) - 1u));
        const uint32_t sh = (g.U8(rd + 46u) << 2) & 31u;
        g.W32(rd + 48u, (g.U32(rd + 48u) & ~(15u << sh)) | ((g.U8(rd + 47u) & 15u) << sh));
        tr.swingSpent = true;
    }
    uint32_t flags = (g.U8(rd + 60u) & 0x40u) ? 32u : 0u;
    if (g.U8(rd + 60u) == 32 && !F::WeaponSteal(g, c, me, t, side, flags)) return false; // 0x800C0808
    if (flags & 0x80u) {
        tr.stole = true; // ReachTest's `flags & 0x80`: the steal took the blow's place
        return !g.Faulted();
    }
    if (snatch) { // a grab is not a blow: no ApplyHit (the punch stands; FightUpdate offers WeaponSteal again)
        tr.missed = true;
        tr.parried = (flags & 0x40u) != 0;
        return !g.Faulted();
    }
    // the blow: the node's damage x strength for this one call (OURS), ApplyHit with the contact as the reach bit
    const float s = std::isfinite(strength) ? std::clamp(strength, 0.1f, 4.0f) : 1.0f;
    tr.used = tr.damage;
    if (s != 1.0f) tr.used = static_cast<uint16_t>(std::clamp<long>(std::lround(tr.damage * s), 1, 0x7FFF));
    {
        uint32_t base = tr.used;
        if (g.S8(r + 0x23Cu) & 0x80) base *= 3u;
        const uint32_t own = g.U8(rd + 15u) < 97u ? 96u : g.U8(rd + 15u);
        tr.expected = static_cast<int>((static_cast<int64_t>(base) * own * g.U8(rd + 12u)) >> 14);
    }
    if (tr.used != tr.damage) g.W16(nd + 8u, tr.used);
    uint32_t v0 = 0;
    const bool ok = F::ApplyHit(g, c, me, t, side, flags | 3u, v0);
    if (tr.used != tr.damage) g.W16(nd + 8u, tr.damage);
    if (!ok) return false;
    tr.applied = true;
    return !g.Faulted();
}

} // namespace
} // namespace rr::game
