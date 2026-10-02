// Combat, ported from our own disassembly of RASHCDG.BIN - see fight.h. Every function carries
// the address it transcribes; comments name the instruction a non-obvious detail comes from.
#include "game/sim/fight.h"

#include "game/sim/ai.h"
#include "game/sim/fixed.h"

namespace rr::sim::fight {
namespace {

constexpr uint32_t U(int32_t v) { return static_cast<uint32_t>(v); }
constexpr int32_t S(uint32_t v) { return static_cast<int32_t>(v); }
inline int32_t Iabs(int32_t x) {
    const uint32_t s = U(x >> 31);
    return S((U(x) + s) ^ s);
}
inline int32_t MulLo(int32_t a, int32_t b) {
    return S(static_cast<uint32_t>(static_cast<int64_t>(a) * static_cast<int64_t>(b)));
}
inline int32_t MipsDiv(int32_t n, int32_t d) {
    if (d == 0) return n >= 0 ? -1 : 1;
    if (U(n) == 0x80000000u && d == -1) return n;
    return n / d;
}

// Entity / rider / riderDef offsets used below.
constexpr uint32_t kRider = 0x354, kPartner = 0x358, kDef = 0x43C, kNode = 0x440, kHandle = 0xAC;
constexpr uint32_t kFlagsA = 0x230, kFlagsB = 0x234, kFlagsC = 0x238;
constexpr uint32_t kDepth = 0x3B2, kStack = 0x3B4;
constexpr uint32_t kBit27 = 0x08000000u;

uint32_t Gs(GuestRam& g) { return g.U32(kGameStatePtr); }
uint32_t Players(GuestRam& g) { return g.U32(Gs(g) + 0x30u); }
uint32_t Pool0(GuestRam& g, uint32_t slot) { return g.U32(kPool0Ptr) + 1096u * slot; }
uint32_t Category(GuestRam& g, uint32_t stance) { return g.U16(kStanceTab + 8u * (stance & 0xFFFFu) + 2u); }
uint32_t Rec(GuestRam& g, uint32_t i) { return g.U32(kFightRecPtr) + 12u * i; }
uint32_t NodeAt(GuestRam& g, uint32_t rec, uint32_t n) { return g.U32(Rec(g, rec) + 8u) + 12u * n; }
bool IsPlayer(GuestRam& g, uint32_t e) { return g.U16(e + kHandle) < Players(g); }
// The human's half: neither the AI-driven bit nor rider +0x23C bit 6.
bool Gated(GuestRam& g, uint32_t me, uint32_t r) {
    return (g.U32(me + kFlagsA) & kBit27) != 0 || (g.U8(r + 0x23C) & 0x40u) != 0;
}
// The top command slot, `e + 0x3B4 + 8 * (s8)e[0x3B2]`.
uint32_t TopSlot(GuestRam& g, uint32_t e) {
    return e + kStack + 8u * U(static_cast<int32_t>(g.S8(e + kDepth)));
}
// Road-lateral width of a bike's own side (CanEngage 0x800BC434, BackOff 0x800BB92C).
int32_t EdgeWidth(GuestRam& g, uint32_t t) {
    if (g.S32(t + 0x174) == 0 || g.U16(t + 0x16A) != 0) return 0x39999;
    const int32_t a = g.S16(t + 0x1A4);
    const int32_t b = g.S16(t + 0x198);
    const int32_t neg = S(0u - (g.U32(t + 0x158) >> 31));
    return MulLo(g.S32(t + 0x1A8), a + (neg & (b - a)));
}
int32_t Project(GuestRam& g, uint32_t p, uint32_t axis, uint32_t q) {
    int32_t a[3], o[3];
    int16_t n[3];
    for (uint32_t k = 0; k < 3; ++k) {
        a[k] = g.S32(p + 4u * k);
        o[k] = g.S32(q + 4u * k);
        n[k] = g.S16(axis + 2u * k);
    }
    return AiProject(a, n, o); // RASHCDG 0x800B6AAC, ported (ai.h)
}
// A pad record's stamp for action n's control (`m` = 0 control, 1 modifier).
int32_t Stamp(GuestRam& g, uint32_t pad, uint32_t ctl) {
    const uint32_t tbl = g.U32(pad + 184u);
    const uint32_t slot = g.U32(tbl + 4u * ctl);
    return g.S32(pad + 8u * slot + 20u);
}

} // namespace

// ============================================================================ the input

int32_t Edge(GuestRam& g, uint32_t pad, int32_t n) { // 0x800C2178
    const uint32_t m = kComboMap + 4u * U(n);
    const int32_t st = Stamp(g, pad, g.U16(m));
    if (st <= 0) return 0;
    if (g.S32(Gs(g) + 32u) < S(g.U32(pad) - U(st))) return 0;
    const uint32_t mod = g.U16(m + 2u);
    if (mod == 15) return 1;
    return Stamp(g, pad, mod) > 0 ? 1 : 0;
}

int32_t EdgeReleased(GuestRam& g, uint32_t pad, int32_t n) { // 0x800C2070
    const uint32_t m = kComboMap + 4u * U(n);
    const int32_t st = Stamp(g, pad, g.U16(m));
    if (st >= 0) return 0;
    if (!(S(0u - U(st)) < 75)) return 0;
    const uint32_t mod = g.U16(m + 2u);
    if (mod == 15) return 1;
    return Stamp(g, pad, mod) > 0 ? 1 : 0;
}

int32_t Held(GuestRam& g, uint32_t pad, int32_t n) { // 0x800C2100
    return Stamp(g, pad, g.U16(kComboMap + 4u * U(n))) > 0 ? 1 : 0;
}

uint32_t Released(GuestRam& g, uint32_t pad, int32_t n) { // 0x800C213C
    return U(Stamp(g, pad, g.U16(kComboMap + 4u * U(n)))) >> 31;
}

bool CombatDecode(GuestRam& g, Callees& c, uint32_t pad, uint32_t bike, uint32_t sp, int32_t& v0) {
    const uint32_t rd = g.U32(bike + kDef);                     // 0x800C236C
    int32_t cmd = -1;
    bool taunt = false;
    if (Edge(g, pad, 8)) cmd = 77;
    else if (Edge(g, pad, 4)) cmd = 75;
    else if (Edge(g, pad, 3)) cmd = 71;
    else if (Edge(g, pad, 7)) {
        const uint32_t w = g.U8(rd + 46u);
        if (w == 9) cmd = 38;
        else if (w == 5 || w == 1) cmd = 148;
        else if (((w - 6u) & 0xFFu) < 3u) taunt = true;
    } else if (Edge(g, pad, 5)) {
        const uint32_t w = g.U8(rd + 46u);
        if (w - 2u < 2u) cmd = 148;                             // 0x800C2424
        else if (w == 0 || w == 8) cmd = 148;
        else if (((w - 6u) & 0xFFu) < 2u) taunt = true;
    } else if (Edge(g, pad, 6)) {
        const uint32_t w = g.U8(rd + 46u);
        if (w == 4 || w == 6 || w == 7) cmd = 148;
        else if (w == 8) taunt = true;
    } else if (Edge(g, pad, 1)) {
        const uint32_t w = g.U8(rd + 46u);
        if (w == 9) cmd = 32;
        else if (((w - 6u) & 0xFFu) < 3u) taunt = true;
        else cmd = 142;
    } else if (Edge(g, pad, 2)) {
        const uint32_t w = g.U8(rd + 46u);
        if (w == 9) cmd = 36;
        else if (((w - 6u) & 0xFFu) < 3u) taunt = true;
        else cmd = 146;
    }
    if (taunt) {                                                // 0x800C2540
        if (!c.PlaySound3D(g.S32(bike + 184u), g.S32(bike + 192u), 78, 0)) return false;
        v0 = 0;
        return true;
    }
    if (cmd < 0) { v0 = 0; return true; }
    g.W8(rd + 60u, static_cast<uint8_t>(cmd));                  // 0x800C2534
    if (!FightPush(g, c, bike, sp - 40u)) return false;        // 0x800C2558
    g.W8(g.U32(bike + kRider) + 0x23Du, 0);                     // 0x800C2568
    v0 = 1;
    return true;
}

bool FightPush(GuestRam& g, Callees& c, uint32_t e, uint32_t sp) { // 0x800C1DD4
    const uint32_t rd = g.U32(e + kDef);
    if (g.S32(rd + 40u) != 0) return true;
    if (!(g.U8(rd + 39u) < 248u)) return true;
    const uint32_t frame = sp - 32u;
    uint16_t tgt = 0;
    if (g.U32(e + kNode) == 0) {                                // 0x800C1E1C
        tgt = Pick(g, g.U32(e + kPartner) + kHandle, 1);
        if (g.U32(g.U32(e + kRider) + 0x228u) & 0x00800000u) tgt = g.U16(g.U32(e + kPartner) + kHandle);
    } else if ((g.U8(g.U32(e + kRider) + 0x23Cu) & 0x10u) && (g.U32(g.U32(e + kRider) + 0x228u) & 0x00800000u)) {
        tgt = g.U16(g.U32(e + kPartner) + kHandle);             // 0x800C1E94
    } else {
        const uint32_t h = g.U16(e + kHandle);
        uint32_t t = 0;
        const uint16_t last = g.U16(kLastSlot + 2u * h);
        if (last != kNoTarget && g.S32(Gs(g) + 16u) - 59 < g.S32(kLastTime + 4u * h)) {
            t = g.U32(kPool0Ptr) + 1096u * last;                // 0x800C1F14
            int32_t a = g.S32(e + 504u) - g.S32(t + 504u);
            int32_t b = g.S32(e + 512u) - g.S32(t + 512u);
            a = Iabs(a >> 16) ;
            b = Iabs(b >> 16);
            // `sra ..,16` then the add/xor absolute value of the shifted word (0x800C1F28..0x800C1F4C)
            int32_t big = a, small = b;
            if (big < small) { big = b; small = a; }
            const int32_t s15 = small + (small >> 1);
            const int32_t d = (big - (big >> 5) - (big >> 7)) + (s15 >> 2) + (s15 >> 6);
            if (!(d < 2)) t = 0;
        }
        tgt = (t == 0) ? Pick(g, e + kHandle, 1) : g.U16(kLastSlot + 2u * h);
    }
    const uint32_t top = e + 956u + 8u * U(static_cast<int32_t>(g.S8(e + kDepth))) - 8u; // 0x800C1FE8
    if (g.U16(top) == 16 && g.U16(top + 2u) == tgt) return true;
    uint8_t cmd[8];
    g.ReadBlock(frame + 16u, cmd, 8);                           // bytes 4..7: whatever the frame holds
    cmd[0] = 16; cmd[1] = 0;
    cmd[2] = static_cast<uint8_t>(tgt); cmd[3] = static_cast<uint8_t>(tgt >> 8);
    return c.PushCommand(cmd, frame + 16u, 2, e);               // 0x800C2018
}

uint16_t Pick(GuestRam& g, uint32_t hf, uint32_t mask) { // 0x8008B428
    uint32_t best = kNoTarget;                                  // t7
    int32_t bestD = 0x7FFF0000;                                 // t3
    const int32_t x = g.S32(hf + 12u), z = g.S32(hf + 20u);
    const uint16_t me = g.U16(hf);
    const uint32_t players = Players(g);
    int32_t col = ((x - g.S32(0x800CCF98u)) >> 21) + 11;        // t6
    int32_t row = ((z - g.S32(0x800CCFA0u)) >> 21) + 11;        // t4
    if (players != 1 && !(U(col) < 24u && U(row) < 24u)) {
        col = ((x - g.S32(0x800CCF9Cu)) >> 21) + 11;
        row = ((z - g.S32(0x800CCFA4u)) >> 21) + 11;
        row = S(U(row) + (24u << ((U(row >> 31)) & 31u)));     // 0x8008B4E8: + 24, or + 0 when negative
    }
    int32_t n = 1;                                              // s0
    const uint32_t hi = (row < 24 ? 0u : 1u) & ((row < S(24u << ((players - 1u) & 31u))) ? 1u : 0u);
    const uint32_t half = (0u - hi) & 0x18u;                    // s8
    row -= S(half);
    const uint32_t base0 = g.U32(kPool0Ptr), base1 = g.U32(kPool1Ptr);
    while (U(col) < 24u || U(row) < 24u || U(col + n) < 24u || U(row + n) < 24u) {
        const int32_t r0 = row < 0 ? 0 : row;
        const int32_t c0 = col < 0 ? 0 : col;
        const int32_t cEnd = (col + n - 24 < 0) ? col + n : 24;
        const int32_t rEnd = (row + n - 24 < 0) ? row + n : 24;
        for (int32_t r = r0; r < rEnd; ++r) {
            for (int32_t cc = c0; cc < cEnd; ++cc) {
                uint32_t k = g.U8(0x800CD0B0u + U(cc) + U(S(half) + r) * 24u);
                while (k != 128) {
                    const uint32_t hb = g.U8(0x800CCFA9u + 2u * k);
                    const uint32_t pool = hb >> 5;
                    if (mask & (1u << pool)) {
                        uint32_t h = 0, ex = 0, ez = 0;
                        bool cand = false;
                        if (pool == 0) {
                            const uint32_t E = base0 + 1096u * hb;
                            h = g.U16(E + 172u);
                            if (h != me) { ex = E + 184u; ez = E + 192u; cand = true; }
                        } else if (pool == 1) {
                            const uint32_t E = base1 + 628u * (hb & 31u);
                            h = g.U16(E + 172u);
                            if (h != me && g.S32(E + 604u) == 4) { ex = E + 184u; ez = E + 192u; cand = true; }
                        } else {
                            const uint32_t pt = 0x800CE4D0u + 16u * pool;
                            const uint32_t E = g.U32(pt) + MulLo(S(g.U32(pt + 4u)), S(hb & 31u)) +
                                               g.U32(0x800CCA68u + 4u * (pool - 2u));
                            h = g.U16(E);
                            if (h != me) { ex = E + 12u; ez = E + 20u; cand = true; }
                        }
                        if (cand) {
                            const int32_t dx = Iabs(g.S32(ex) - x);
                            const int32_t dz = Iabs(g.S32(ez) - z);
                            const int32_t mn = dx < dz ? dx : dz;
                            const int32_t d = (dx + dz) - (S(U(mn) + (U(mn) >> 31)) >> 1);
                            if (d < bestD) { bestD = d; best = h; }
                        }
                    }
                    k = g.U8(0x800CCFA8u + 2u * k);
                }
            }
        }
        n += 2;
        --col;
        --row;
        if ((best & 0xFFFFu) != kNoTarget) return static_cast<uint16_t>(best);
    }
    return static_cast<uint16_t>(best);
}

bool ComboInput(GuestRam& g, uint32_t pad, uint32_t bike) { // 0x800C258C
    const uint32_t r = g.U32(bike + kRider);
    bool go = true;                                             // sp+16
    const uint32_t rec = Rec(g, g.U8(r + 0x239u));              // sp+20
    uint32_t a = g.U32(rec + 4u);                               // s5: the edge block
    if (g.U8(r + 0x23Cu) & 2u) return true;
    const int32_t acc = S(g.U32(r + 0x230u) + g.U32(Gs(g) + 32u)); // s4
    int32_t base = g.S32(r + 0x234u);                           // s7
    g.W32(r + 0x230u, U(acc));
    if (g.U8(r + 0x238u) < g.U8(r + 0x23Au)) g.W8(r + 0x238u, g.U8(r + 0x23Au));
    const uint32_t count = g.U8(rec + 2u);
    for (uint32_t i = 0; i < count; ++i, a += 12u) {
        if (!go) return true;
        const uint32_t w = g.U32(a + 8u);
        const uint32_t slot = (w >> 8) & 0x3Fu;
        const uint32_t prevNode = g.U8(r + 0x23Du + slot);
        const int32_t t0 = S(g.U32(a) - U(base));
        const int32_t s3 = S(g.U32(a + 4u) - U(base));
        bool live = false;
        if (S(g.U8(r + 0x238u)) < S(slot) && prevNode == ((w >> 14) & 0x3Fu) && !(acc < t0)) live = acc < s3;
        const uint32_t bit = 1u << (i & 31u);
        auto accept = [&]() {
            g.W32(r + 0x230u, 0);
            g.W8(r + 0x238u, static_cast<uint8_t>(slot));
            g.W32(r + 0x234u, g.U32(a + 4u));
            const uint32_t w2 = g.U32(a + 8u);
            g.W8(r + 0x23Eu + ((w2 >> 8) & 0x3Fu), static_cast<uint8_t>((w2 >> 20) & 0x3Fu));
        };
        auto skipped = [&]() { // 0x800C2748 / 0x800C2838
            if ((S(g.U8(r + 0x23Du)) >> (i & 31u)) & 1) return;
            const uint32_t sl = (g.U32(a + 8u) >> 8) & 0x3Fu;
            if (S(g.U8(r + 0x238u)) < S(sl) && !(s3 < acc)) return;
            g.W8(r + 0x23Du, static_cast<uint8_t>(g.U8(r + 0x23Du) | bit));
        };
        const uint32_t op = (w & 0xFFu) >> 4;
        switch (op) {
        case 0:
            if (live && EdgeReleased(g, pad, S(w & 0xFu))) {
                accept();
                g.W8(r + 0x23Du, static_cast<uint8_t>(g.U8(r + 0x23Du) | bit));
            } else {
                skipped();
            }
            break;
        case 1:
        case 2:
            if (live) {
                if (Released(g, pad, S(w & 0xFu))) {
                    accept();
                    g.W8(r + 0x23Du, static_cast<uint8_t>(g.U8(r + 0x23Du) | bit));
                    break;
                }
                if (Held(g, pad, S(w & 0xFu))) {
                    const uint32_t w2 = g.U32(a + 8u);
                    g.W8(r + 0x23Eu + ((w2 >> 8) & 0x3Fu), static_cast<uint8_t>(w2 >> 26));
                    break;
                }
            }
            skipped();
            break;
        case 3:
            if ((S(g.U8(r + 0x23Du)) >> (i & 31u)) & 1) break;
            if (!Held(g, pad, S(w & 0xFu))) {
                accept();
                g.W8(r + 0x23Du, static_cast<uint8_t>(g.U8(r + 0x23Du) | bit));
            }
            go = false;                                         // 0x800C2998
            break;
        case 4:
            if ((S(g.U8(r + 0x23Du)) >> (i & 31u)) & 1) break;
            if (S(g.U8(r + 0x238u)) < S(slot) && (prevNode & 0xFFu) == ((w >> 14) & 0x3Fu)) {
                accept();
                base = g.S32(r + 0x234u);                       // 0x800C290C: s7 reloaded
                g.W8(r + 0x23Du, static_cast<uint8_t>(g.U8(r + 0x23Du) | bit));
            }
            break;
        default:
            break;
        }
    }
    return true;
}

// ============================================================================ the fight

bool FightStep(GuestRam& g, Callees& c, uint32_t me, uint32_t& v0) { // 0x800C09B0
    const uint32_t r = g.U32(me + kRider);
    uint32_t n = g.U8(r + 0x23Eu + g.U8(r + 0x23Au));
    uint32_t k = 0;
    uint32_t done = 0;
    bool skipEvent = false;
    if (n == kNodeNone) {
        if (!c.ClipDone(g.U32(r + 0x21Cu), done)) return false;
        if (done != 0) k = 8; else skipEvent = true;
    } else {
        if (!c.ClipDone(g.U32(r + 0x21Cu), done)) return false;
        if (done == 0) {
            if (g.U8(r + 0x222u) != 0) { v0 = 0; return true; }
            const uint32_t nd = NodeAt(g, g.U8(r + 0x239u), n);
            const uint8_t fired = (g.S32(g.U32(r + 0x21Cu) + 16u) < S(g.U8(nd + 11u))) ? 0 : 1;
            g.W8(r + 0x222u, fired);                            // 0x800C0A8C
            k = fired & 1u;
        } else {
            const uint32_t i = g.U8(r + 0x23Au) + 1u;
            const uint32_t nx = g.U8(r + 0x23Eu + i);
            g.W8(r + 0x23Au, static_cast<uint8_t>(i));          // 0x800C0A1C
            if (nx == kNodeNone) k = 4; else { k = 2; n = nx; }
        }
    }
    if (!skipEvent) {
        const uint32_t fl = (g.U32(r + 0x228u) & kBit27) ? 272u : 16u;
        const uint32_t rec = g.U8(r + 0x239u);
        uint32_t ev = 0, p = fl;
        bool send = true;
        if (k & 2u) ev = g.U16(NodeAt(g, rec, n));
        else if (k & 4u) {
            ev = g.U16(NodeAt(g, rec, n) + 2u);
            if (ev == kNoTarget) { ev = g.U16(Rec(g, rec)); p |= 2u; }
        } else if (k & 8u) ev = g.U16(Rec(g, rec));
        else send = false;
        if (send) {
            uint32_t ignored = 0;
            if (!c.StanceEvent(ev, r, p, ignored)) return false;
        }
    }
    v0 = k & 1u;
    return !g.Faulted();
}

uint32_t FightPickRecord(GuestRam& g, uint32_t me) { // 0x800BF978
    const uint32_t rd = g.U32(me + kDef);
    const uint32_t r = g.U32(me + kRider);
    const uint32_t cmd = g.U8(rd + 60u);
    uint32_t s0 = 0;
    uint32_t a2 = cmd & 0x1Fu;
    auto armed = [&](uint32_t inRange, uint32_t pair, uint32_t other, uint32_t otherRow) {
        const uint32_t c3 = g.U8(rd + 60u);
        if (((c3 + 114u) & 0xFFu) < 4u) {                       // 142..145
            s0 = inRange;
            if (c3 == 143) a2 = 22;                             // 0x800BFB54
            return;
        }
        if (((c3 + 110u) & 0xFFu) < 2u) { s0 = pair; return; }  // 146, 147
        s0 = other;
        if (otherRow != 0) a2 = otherRow;
    };
    if (cmd & 0x40u) {
        const uint32_t i = cmd - 71u;
        if (i < 7u) {
            if (i < 4u) s0 = 10;
            else if (i < 6u) s0 = 11;
            else { s0 = 17; a2 = 23; }
        } else s0 = 17;
    } else if (cmd & 0x20u) {
        const uint32_t i = g.U8(rd + 60u) - 32u;
        if (i < 7u) {
            if (i < 4u) s0 = 0;
            else if (i < 6u) s0 = 1;
            else { s0 = 16; a2 = 23; }
        } else s0 = 16;
    } else {
        const uint32_t w = g.U8(rd + 46u);
        switch (w) {
        case 0: armed(6, 7, 14, 0); break;
        case 1: case 5: armed(4, 5, 13, 23); break;
        case 2: case 3: armed(2, 3, 12, 23); break;
        case 4: {                                               // 0x800BFA8C: no 143 row
            const uint32_t c3 = g.U8(rd + 60u);
            if (((c3 + 114u) & 0xFFu) < 4u) s0 = 8;
            else if (((c3 + 110u) & 0xFFu) < 2u) s0 = 9;
            else s0 = 15;
            break;
        }
        case 6: case 7: s0 = 18; a2 = 25u - (a2 & 1u); break;
        case 8: s0 = 19; a2 = 25u - (a2 & 1u); break;
        default: break;
        }
    }
    if (g.U8(r + 0x23Cu) & 0x20u) {                             // 0x800BFBA0: the passenger set
        if ((s0 & 0xFFu) == 19) { s0 = 12; a2 = 23; }
        s0 += 20;
    }
    g.W8(r + 0x239u, static_cast<uint8_t>(s0));
    g.W8(r + 0x23Au, 0);
    if (!(g.U32(me + kFlagsA) & kBit27) && !(g.U8(r + 0x23Cu) & 0x40u)) {
        g.W8(r + 0x23Eu, 0);
        for (uint32_t i = 1; i < 6; ++i) g.W8(r + 0x23Eu + i, kNodeNone);
    } else {
        for (uint32_t i = 0; i < 6; ++i)                        // SLUS 0x8001E08C, bytes
            g.W8(r + 0x23Eu + i, g.U8(kCannedQueues + a2 * 6u + i));
    }
    return s0 & 0xFFu;
}

bool FightBegin(GuestRam& g, Callees& c, uint32_t me, uint32_t side) { // 0x800C12D8
    const uint32_t r = g.U32(me + kRider);
    g.W8(r + 0x23Cu, static_cast<uint8_t>(g.U8(r + 0x23Cu) & 0xFDu));
    FightPickRecord(g, me);
    g.W32(r + 0x230u, 0);
    g.W32(r + 0x234u, 0);
    g.W8(r + 0x238u, 0);
    const int32_t n = g.S8(r + 0x23Eu + g.U8(r + 0x23Au));     // `lb` at 0x800C1334
    const uint32_t ev = g.U16(g.U32(Rec(g, g.U8(r + 0x239u)) + 8u) + U(n * 12));
    uint32_t ignored = 0;
    return c.StanceEvent(ev, r, side | 0x10u, ignored);
}

bool FightStart(GuestRam& g, Callees& c, uint32_t me, uint32_t t) { // 0x800C110C
    const uint32_t r = g.U32(me + kRider);
    uint32_t side = 0;
    g.W8(r + 0x23Cu, static_cast<uint8_t>(g.U8(r + 0x23Cu) & 0xFEu));
    if (t == 0 && g.U32(me + kPartner) == 0) {
        side = (GuestRand(g) & 1u) << 8;                        // 0x800C1150
    } else if (g.U32(me + kNode) == 0) {
        if (t == g.U32(me + kPartner)) side |= 0x100u;
    } else if (g.U32(me + kPartner) != 0) {
        if (t != g.U32(me + kPartner)) side |= 0x100u;
    } else {
        const uint32_t tr = g.U32(t + 0x168u), mr = g.U32(me + 0x168u);
        if (tr != mr || ((tr >> 16) != 0 && g.U32(t + 0x150u) != g.U32(me + 0x150u)))
            (void)Project(g, t + 184u, me + 432u, me + 184u);   // 0x800C11E4: the result is discarded
        if (Project(g, t + 184u, me + 432u, me + 184u) < 0) side |= 0x100u;
    }
    return FightBegin(g, c, me, side);
}

bool FightEnd(GuestRam& g, Callees& c, uint32_t r, uint32_t& v0) { // 0x800C1014
    const uint32_t B = g.U32(r + 0x254u);
    g.W8(r + 0x239u, kNoRecord);
    g.W8(r + 0x23Au, 0);
    if (!(g.U32(B + kFlagsA) & kBit27) && !(g.U8(r + 0x23Cu) & 0x40u)) {
        const uint32_t top = B + 8u * U(static_cast<int32_t>(g.S8(B + kDepth)) - 1);
        if (g.U16(top + 956u) == 16) {
            const uint32_t h = g.U16(B + kHandle);
            if (h < Players(g)) {
                g.W16(kLastSlot + 2u * h, g.U16(top + 958u));
                g.W32(kLastTime + 4u * h, g.U32(Gs(g) + 16u));
            }
            if (!c.PopCommand(B)) return false;                 // 0x800C10CC
        }
    }
    const uint32_t a = g.U32(r + 0x21Cu);
    g.W32(a + 36u, g.U32(a + 36u) & ~0x80u);                    // no null test
    const uint8_t b = static_cast<uint8_t>(g.U8(r + 0x23Cu) & 0xFDu);
    g.W8(r + 0x23Cu, b);
    v0 = b;
    return !g.Faulted();
}

void PickWeapon(GuestRam& g, uint32_t rd) { // 0x800B9340
    const uint32_t c = g.U8(rd + 60u);
    if (!(c & 0x80u)) {
        if (c & 0x20u) { g.W8(rd + 46u, 9); g.W8(rd + 47u, 0); }
        return;
    }
    const uint32_t own = g.U16(rd + 44u);
    if ((own & 0x1FFu) == 0) {
        g.W8(rd + 46u, 9);
        g.W8(rd + 47u, 0);
        g.W8(rd + 60u, 32);
        return;
    }
    const uint32_t order = 0x80052EECu + 36u * (g.U8(rd + 1u) & 0xFu);
    uint32_t w = 0;
    for (uint32_t i = 0;; ++i) {
        w = g.U8(order + i);
        g.W8(rd + 46u, static_cast<uint8_t>(w));
        if ((S(own) >> (w & 31u)) & 1) break;
        if (i > (1u << 20)) return; // unreachable: the mask is non-zero and the order is a permutation of 0..8
    }
    g.W8(rd + 47u, static_cast<uint8_t>(w < 8u ? (g.U32(rd + 48u) >> ((w << 2) & 31u)) & 0xFu : 0u));
}

void NextMove(GuestRam& g, uint32_t rd) { // 0x800B92C0
    const uint32_t r = GuestRand(g) & 0x7Fu;
    uint32_t sum = 0;
    uint32_t i = 0;
    for (;; ++i) {
        sum += g.U8(kMoveWeights + i);
        if (!(sum < r)) break;
    }
    g.W8(rd + 60u, g.U8(rd + 52u + i));
    PickWeapon(g, rd);
}

bool FightRestart(GuestRam& g, Callees& c, uint32_t me, uint32_t t, uint32_t side) { // 0x800C122C
    (void)t;
    uint32_t rd = g.U32(me + kDef);
    if ((g.U8(rd + 61u) & 3u) == 2u) {
        NextMove(g, rd);
        rd = g.U32(me + kDef);
    }
    if (g.U8(rd + 60u) & 0x80u) {
        const uint32_t w = g.U8(rd + 46u);
        if ((w == 0 || w == 4) && g.U32(kAnimPool + 8u) == g.U32(kAnimPool + 12u)) g.W8(rd + 60u, 32);
    }
    return FightBegin(g, c, me, side);
}

int32_t CanEngage(GuestRam& g, uint32_t me, uint32_t t, int32_t along) { // 0x800BC1EC
    if (g.S16(t + 0x140u) == 0) return 0;
    const uint32_t mnt = g.U32(g.U32(t + kRider) + 0x25Cu);
    if (!(mnt < 2u)) return 0;
    const bool tPlayer = IsPlayer(g, t);
    if (!tPlayer && mnt != 1) return 0;
    if (g.U8(Gs(g) + 57u) == 3 && tPlayer) return 1;
    int32_t lim = g.S32(0x80052F84u);
    if (tPlayer) lim = S(U(lim) << 2);
    int32_t d;
    const uint32_t tr = g.U32(t + 0x168u), mr = g.U32(me + 0x168u);
    if (tr == mr && ((tr >> 16) == 0 || g.U32(t + 0x150u) == g.U32(me + 0x150u))) {
        d = S(g.U32(t + 0x158u) - g.U32(me + 0x158u));
        if (g.S32(me + 0x16Cu) < 0) d = S(0u - U(d));
    } else {
        d = Project(g, t + 184u, me + 432u, me + 184u);
    }
    if (g.U32(t + kPartner) != 0 && d < 0)
        d = S(U(d) + g.U32(t + 304u) + g.U32(g.U32(t + kPartner) + 304u));
    if (lim < Iabs(d)) return 0;
    int32_t lim2 = g.S32(0x80052F80u);
    if (tPlayer) lim2 = S(U(lim2) << 2);
    if (lim2 < Iabs(along)) return 0;
    if (g.U32(me + kFlagsA) & kBit27) {
        const uint32_t k = g.U8(kClassBlocks + 36u * (g.U8(g.U32(me + kDef) + 1u) & 0xFu) + 18u);
        int32_t v = MulLo(S(k), g.S32(g.U32(me + 0x22Cu) + 224u));
        if (v < 0) v += 127;
        if (g.S32(t + 480u) < (v >> 7)) return 0;
    }
    const int32_t w = EdgeWidth(g, t);
    const int32_t e = S(U(Iabs(g.S32(t + 0x158u))) - U(w));
    if ((g.U32(me + kFlagsA) & kBit27) && g.U16(t + 0x16Au) != 1 && g.S32(kK) < e) return 0;
    return 1;
}

int32_t BackOff(GuestRam& g, uint32_t me, uint32_t t, int32_t x) { // 0x800BB8FC
    if (g.S16(t + 0x140u) == 0) return 0;
    if (g.S32(g.U32(t + kRider) + 0x25Cu) != 1) return 0;
    const int32_t w = EdgeWidth(g, t);
    const int32_t e = S(U(Iabs(g.S32(t + 0x158u))) - U(w));
    if (x < g.S32(kK + 4u)) return 0;
    if (g.S32(kK + 12u) < x) return 0;
    if (g.S32(kK) < e) return 0;
    if (g.S32(g.U32(me + 0x22Cu) + 224u) < g.S32(t + 480u) && !IsPlayer(g, t)) return 0;
    return 1;
}

bool FightContinue(GuestRam& g, Callees& c, uint32_t me, uint32_t t, int32_t along, uint32_t s, uint32_t sp,
                   int32_t& v0) { // 0x800C0BE8
    if (!(s & 1u)) {
        if (g.U32(me + kNode) != 0 && BackOff(g, me, t, S(0u - U(along)))) {
            const uint32_t at = sp - 40u + 16u;
            uint8_t cmd[8];
            g.ReadBlock(at, cmd, 8);
            const uint16_t h = g.U16(t + kHandle);
            cmd[0] = 6; cmd[1] = 0;
            cmd[2] = static_cast<uint8_t>(h); cmd[3] = static_cast<uint8_t>(h >> 8);
            if (!c.PushCommand(cmd, at, 2, me)) return false;   // 0x800C0C3C
            v0 = 0;
            return true;
        }
    } else {
        const uint32_t d = (s & 2u) ? g.U32(g.U32(t + kPartner) + kDef) : g.U32(t + kDef);
        const uint32_t hp = g.U8(d + 15u);
        const uint32_t k = g.U8(g.U32(me + kDef) + 61u);
        if ((k & 0xFu) < (k >> 4) && hp != 0) { v0 = 1; return true; }
    }
    const uint32_t r = g.U32(me + kRider);
    if (g.U8(r + 0x23Cu) & 0x40u) {
        uint32_t ignored = 0;
        if (!c.StanceEvent(77, r, 1, ignored)) return false;
    }
    if (!c.PopCommand(me)) return false;
    v0 = 0;
    return true;
}

int32_t OtherOnSide(GuestRam& g, uint32_t t, uint32_t h, int32_t bias) { // 0x800BC618
    uint32_t other = 0;
    const uint32_t th = g.U16(t + kHandle);
    bool viaStack = true;
    if (th < Players(g)) {
        const uint32_t m = g.U16(kAttackers + 2u * th);
        if (m != 0) {
            viaStack = false;
            const uint32_t rest = m & (0xFFFFu - (1u << (((h & 0xFFFFu) - 1u) & 31u)));
            if (rest != 0 && (rest & (0u - rest)) == rest) {
                // mtc2 LZCS / mfc2 LZCR (0x800BC698): the bit's index as 31 - leading zeros
                uint32_t p = 0;
                while (((rest >> p) & 1u) == 0) ++p;
                other = g.U32(kPool0Ptr) + 1096u * ((p + 1u) & 0xFFFFu);
            }
        }
    }
    if (viaStack) {
        const uint32_t top = TopSlot(g, t);
        const uint32_t op = g.U16(top);
        if ((op - 6u) < 2u || (op - 14u) < 4u) {
            const uint32_t h2 = g.U16(top + 2u);
            if (h2 != (h & 0xFFFFu)) other = g.U32(kPool0Ptr) + 1096u * h2;
        }
    }
    if (other == 0) return 0;
    const int32_t dp = S((g.U32(t + 324u) - g.U32(other + 324u)) << 4);
    if (0x7FFFF < Iabs(dp)) return 0;
    const uint32_t o = g.U32(other + 344u), tt = g.U32(t + 344u);
    int32_t lat = S(o + tt);
    if (S(g.U32(other + 364u) ^ g.U32(t + 364u)) >= 0) lat = S(o - tt);
    return (S(U(lat) ^ U(bias)) >> 31) + 1;
}

bool SideAim(GuestRam& g, Callees& c, uint32_t me, uint32_t t, int32_t b, uint32_t sp) { // 0x800BC4FC
    uint32_t v = U(b);
    if ((g.U16(t + kHandle) >> 5) == 0) {
        const uint32_t p = g.U32(t + kPartner);
        if (p != 0) {
            const uint32_t d = g.U32(t + 364u);
            if (((~d) >> 31) != (U(b) >> 31)) {
                const uint32_t w = g.U32(t + 304u) + g.U32(p + 304u);
                v = S(d) < 0 ? U(b) - w : U(b) + w;
            }
        }
    }
    const uint32_t slice = g.U32(t + 340u);
    const int32_t p1 = MulLo(g.S16(slice + 2u), g.S16(me + 872u));
    const int32_t p2 = MulLo(g.S16(slice + 6u), g.S16(me + 876u));
    v = v + g.U32(t + 344u) - (U(static_cast<int32_t>(g.S16(me + 878u))) << 5);
    const int32_t sum = S(U(p1) << 4) >> 16;
    const int32_t sum2 = S(U(p2) << 4) >> 16;
    if (sum + sum2 < 0) v = 0u - v;
    g.W32(sp + 8u, v);                                          // `sw a0,40(sp)`: the home slot of a2
    return c.SetAimDelta(me, sp + 8u, S(v));                    // 0x800BC600
}

void FightPace(GuestRam& g, uint32_t me, uint32_t t, int32_t along) { // 0x800C0E98
    const int32_t k19 = g.S32(kK + 76u);
    if (k19 < along) {
        g.W32(me + 924u, g.U32(t + 480u) - g.U32(kSpeedTab + 4u * g.U32(Gs(g) + 60u)));
        return;
    }
    if (along < S(0u - U(k19))) {
        g.W32(me + 924u, g.U32(g.U32(me + 0x22Cu) + 224u));
        uint32_t fb = g.U32(me + kFlagsB);
        if (IsPlayer(g, t)) fb |= 0x200u;
        g.W32(me + kFlagsB, fb);
        return;
    }
    const bool near = Iabs(along) < g.S32(kK + 32u);
    if (near && g.U16(t + kHandle) < g.U16(me + kHandle) && (g.U32(t + kFlagsA) & kBit27)) {
        const uint32_t top = TopSlot(g, t);
        if (g.U16(top + 2u) == g.U16(me + kHandle)) {
            const uint32_t op = g.U16(top);
            if (op >= 14u && op < 17u) return;
        }
    }
    const int32_t a = Iabs(along);
    const int32_t h = S(U(a) + (U(a) >> 31)) >> 1;
    const uint32_t use = (near || 0x10000 < h) ? 1u : 0u;
    const int32_t r = S(((0u - use) & (U(h) - 0x10000u)) + 0x10000u);
    const int32_t off = along >= 0 ? S(0u - U(r)) : r;
    g.W32(me + 924u, g.U32(t + 480u) + U(off));
}

bool FightSteer(GuestRam& g, Callees& c, uint32_t me, uint32_t t, int32_t along, uint32_t sp) { // 0x800C0CE8
    const int32_t dv = S(g.U32(t + 480u) - g.U32(me + 480u));
    int32_t w = S(U(Iabs(dv)) << 2);
    const int32_t k19 = g.S32(kK + 76u);
    if (!(k19 < w)) w = k19;
    const int32_t aa = Iabs(along);
    if (aa < w) {
        const uint32_t a1 = g.U32(t + 344u), a0 = g.U32(me + 344u);
        int32_t lat = S(a1 + a0);
        if (S(g.U32(t + 364u) ^ g.U32(me + 364u)) >= 0) lat = S(a1 - a0);
        uint32_t i = 10;
        if (!(g.S32(kK + 32u) < aa)) i = (Iabs(lat) < g.S32(kK + 36u)) ? 9u : 11u;
        if (i == 11 && IsPlayer(g, t))
            if (!c.RiderSpeech(g.U16(me + kHandle), 0)) return false; // 0x800C0DF0
        int32_t b = g.S32(kK + 4u * i);
        if (lat > 0) b = S(0u - U(b));
        if (OtherOnSide(g, t, g.U16(me + kHandle), b) != 0) b = S(0u - U(b));
        if (!SideAim(g, c, me, t, b, sp - 48u)) return false;
    }
    FightPace(g, me, t, along);
    g.W32(me + 916u, 0);
    g.W32(me + 920u, 0);
    g.W32(me + kFlagsB, g.U32(me + kFlagsB) & 0xFFF7FFFFu);
    return !g.Faulted();
}

int32_t ReachTest(GuestRam& g, uint32_t me, uint32_t t, int32_t lat, int32_t along, uint32_t& flags) { // 0x800C159C
    const uint32_t r = g.U32(me + kRider);
    const int32_t dy = S(g.U32(t + 188u) - g.U32(me + 188u));
    const int32_t ady = Iabs(dy);
    const uint32_t nd = NodeAt(g, g.U8(r + 0x239u), g.U8(r + 0x23Eu + g.U8(r + 0x23Au)));
    int32_t s1 = 0;
    if (g.U16(nd + 8u) != 0) s1 = ady < g.S32(me + 312u) ? 1 : 0;
    s1 |= S((flags >> 8) & 1u);
    if (s1 != 0 && !(0xFFFE < Iabs(along))) {
        int32_t d = S(g.U32(me + 636u) - g.U32(t + 636u));
        if (lat > 0) d = S(0u - U(d));
        const int32_t sn = g.S16(kSinTable + ((U(MulLo(d, 163)) >> 12) & 0x3FFCu));
        const int32_t tb = g.S32(t + 312u);
        if (ady < (S(U(tb) + (U(tb) >> 31)) >> 1)) {
            int32_t q = MulLo(tb, 3);
            if (q < 0) q += 3;
            const int32_t m = FixMul(S(U(sn) << 4), S(U(dy) + U(q >> 2)));   // SLUS 0x8001FC90
            const uint32_t reach = U(m) + (U(g.U8(nd + 10u)) << 12) + g.U32(g.U32(me + kDef) + 8u) +
                                   g.U32(me + 304u) + g.U32(t + 304u);
            flags |= (Iabs(lat) < S(reach)) ? 1u : 0u;
        }
    }
    flags |= 2u;
    return s1 != 0 ? ((flags & 0x80u) == 0 ? 1 : 0) : 0;
}

uint16_t HitStance(GuestRam& g, uint32_t r, uint32_t v, int32_t hit, uint32_t& damage) { // 0x800BF860
    const uint32_t nd = NodeAt(g, g.U8(r + 0x239u), g.U8(r + 0x23Eu + g.U8(r + 0x23Au)));
    uint32_t s;
    if (hit != 0) {
        damage = g.U16(nd + 8u);                                // 0x800BF8AC
        s = g.U16(nd + 4u);
    } else {
        s = g.U16(nd + 6u);
    }
    if (g.U8(v + 0x23Cu) & 0x20u) {
        static const uint16_t kTab[6] = {83, 85, 85, 86, 84, 83}; // 0x8005BA04
        const uint32_t i = s - 30u;
        s = i < 6u ? kTab[i] : 87u;
    }
    return static_cast<uint16_t>(s);
}

void NoteHit(GuestRam& g, uint32_t a, uint32_t v) { // 0x800BF424
    uint32_t idx;
    const uint32_t h = g.U16(a + kHandle);
    if (h < Players(g)) idx = h;
    else {
        if (g.U32(a + kNode) != 0) return;
        idx = g.U16(g.U32(a + kPartner) + kHandle) + 2u;
    }
    g.W32(kLastBlow + 12u * idx, a);
    g.W32(kLastBlow + 12u * idx + 4u, v);
    g.W32(kLastBlow + 12u * idx + 8u, g.U32(Gs(g) + 16u));
}

void HitShove(GuestRam& g, uint32_t v, uint32_t node, int32_t rec, int32_t side) { // 0x800BF604
    if (node != g.U8(kShoveNode + U(rec))) return;
    const uint32_t p = kShoveParams + 2u * U(rec);
    g.W32(v + 720u, U(g.U8(p)) << 8);
    g.W32(v + kFlagsB, g.U32(v + kFlagsB) | 0x01000000u);
    const uint32_t w = U(g.U8(p + 1u)) << 10;
    g.W32(v + 656u, w + ((0u - U(side)) & (0u - w - w)));
}

bool HitRumble(GuestRam& g, Callees& c, uint32_t e, int32_t fin) { // 0x800BFD74
    int32_t x = MulLo(fin, 150);
    if (x < 0) x += 127;
    const int32_t a3 = x >> 7;
    const int32_t lo = S(~U(x >> 31)) & a3;
    const int32_t v = 255 - a3;
    const int32_t c3 = lo + ((v >> 31) & v);
    int32_t port;
    if (g.U32(e + kNode) == 0) {
        if (g.U8(g.U32(e + kRider) + 0x23Cu) & 0x40u) port = -1;
        else port = S(g.U16(g.U32(e + kPartner) + kHandle) + 1u + ((Players(g) < 2u ? 1u : 0u) ^ 1u));
    } else {
        port = g.U16(e + kHandle);
    }
    if (port < 0) return true;
    int32_t y = MulLo(fin, 100);
    if (y < 0) y += 127;
    return c.PadMotor(port, y >> 7, 100, c3);
}

void FightStat(GuestRam& g, uint32_t e, int32_t k, int32_t which) { // 0x800BFE58
    uint32_t row;
    const uint32_t h = g.U16(e + kHandle);
    if (h < Players(g)) row = h;
    else {
        if (g.U32(e + kNode) != 0) return;
        row = g.U16(g.U32(e + kPartner) + kHandle) + 2u;
    }
    const uint32_t at = kFightStats + U(k) + U(which) * 4u + row * 36u;
    g.W8(at, static_cast<uint8_t>(g.U8(at) + 1u));
}

int32_t RememberHandle(uint32_t h, uint32_t& set) { // 0x800A8BE0
    uint32_t v = set;
    uint32_t sh = 0;
    const uint32_t want = (h & 0xFFFFu) + 1u;
    while (v != 0) {
        if ((v & 31u) == want) return 1;
        v >>= 5;
        sh += 5;
    }
    if (S(sh) < 30) {
        set |= (want & 31u) << (sh & 31u);
        return 1;
    }
    return 0;
}

bool KnockOff(GuestRam& g, Callees& c, uint32_t v, uint8_t cmd, int32_t side, uint32_t a) { // 0x800BF674
    uint32_t fc = g.U32(v + kFlagsC);
    fc = (g.S32(v + 364u) < 0) ? (fc | 0x00400000u) : (fc & 0xFFBFFFFFu);
    g.W32(v + kFlagsC, fc);
    const uint32_t vr = g.U32(v + kRider);
    g.W32(vr + 0x228u, g.U32(vr + 0x228u) & 0xFFF9FFFFu);
    const bool special = ((cmd - 36u) & 0xFFu) < 2u || cmd == 75 || cmd == 76 || cmd == 146 || cmd == 147;
    if (g.U32(v + kPartner) != 0 || (g.U32(v + kFlagsC) & 0x400u)) {
        const uint32_t add = special ? 0x8000u : ((((0u - U(side)) & 0xFFFE0000u) + 0x40000u) | 0x8000u);
        const uint32_t r2 = g.U32(v + kRider);
        g.W32(r2 + 0x228u, g.U32(r2 + 0x228u) | add);
        g.W32(v + 720u, 0xFFFF0000u);
    } else {
        uint32_t bits;
        if (special) bits = 0x880u;
        else {
            const uint32_t ns = 0u - U(side);
            bits = ((ns & 0x8000u) + 0x8000u) | 0x840u;
            const uint32_t r2 = g.U32(v + kRider);
            g.W32(r2 + 0x228u, g.U32(r2 + 0x228u) | 0x8000u | ((ns & 0xFFFE0000u) + 0x40000u));
        }
        g.W32(v + kFlagsC, (g.U32(v + kFlagsC) & 0xFFFF9FC0u) | bits);
    }
    const uint32_t h = g.U16(a + kHandle);
    if (h < Players(g)) {
        const uint32_t idx = h + (S(1u - h) >> 31 & (1u - h)); // min(h, 1)
        g.W32(kDashFlash + 224u * idx, 1);                      // 0x800BF82C
        if (g.U8(Gs(g) + 4u) & 1u)
            if (!c.Arrest(a, v, 9, a)) return false;               // 0x800BF848
    }
    return true;
}

int32_t StealWindow(GuestRam& g, uint32_t r, uint8_t weapon) { // 0x800C2E9C
    if (g.U8(r + 0x239u) != 0) return 0;
    if (g.S8(r + 0x23Eu + g.U8(r + 0x23Au)) != 1) return 0;
    const uint32_t a = g.U32(r + 0x21Cu);
    const uint32_t b = g.U8(g.U32(a + 4u) + 12u * g.U32(a + 12u));
    const uint32_t clip = g.U32(g.U32(g.U32(a + 40u) + 4u) + 4u * b);
    const int32_t n = static_cast<int16_t>(static_cast<uint16_t>(g.U16(clip + 16u) - 1u));
    const int32_t f4 = S(g.U32(a + 16u) << 2);
    if (!(n < f4)) return 0;
    if (!(f4 < MulLo(n, 3))) return 0;
    if (g.U8(r + 0x23Cu) & 0x20u) return (weapon == 0 || weapon == 2 || weapon == 6 || weapon == 7) ? 1 : 0;
    return 1;
}

void CloseQueue(GuestRam& g, uint32_t r) { // 0x800C2030
    uint32_t i = g.U8(r + 0x23Au) + 1u;
    g.W8(r + 0x23Cu, static_cast<uint8_t>(g.U8(r + 0x23Cu) | 2u));
    for (; S(i) < 6; ++i) g.W8(r + 0x23Eu + i, kNodeNone);
}

bool WeaponSteal(GuestRam& g, Callees& c, uint32_t me, uint32_t t, uint32_t side, uint32_t& flags) { // 0x800BFF04
    const uint32_t s3 = g.U32(me + kRider);
    const uint32_t tr = g.U32(t + kRider);
    const bool any = IsPlayer(g, me) || IsPlayer(g, t) || (g.U8(s3 + 0x23Cu) & 0x20u) || (g.U8(tr + 0x23Cu) & 0x20u);
    if (!any) return true;
    if (Category(g, g.U16(tr + 0x220u)) != 3) return true;
    const uint32_t trd = g.U32(t + kDef);
    if (!(g.U8(trd + 60u) & 0x80u)) return true;
    if (!StealWindow(g, s3, g.U8(trd + 46u))) return true;
    const uint32_t a = g.U32(g.U32(t + kRider) + 0x21Cu);
    const uint32_t b = g.U8(g.U32(a + 4u) + 12u * g.U32(a + 12u));
    const uint32_t clip = g.U32(g.U32(g.U32(a + 40u) + 4u) + 4u * b);
    const int32_t f = g.S32(a + 16u);
    const int32_t n = static_cast<int16_t>(static_cast<uint16_t>(g.U16(clip + 16u) - 1u));
    if (MulLo(n, 3) < S(U(f) << 2)) flags |= 0x40u;
    if (flags & 0x40u) return c.PlaySound3D(g.S32(me + 184u), g.S32(me + 192u), 88, 0); // 0x800C0330
    if (!(n < S(U(f) << 1))) return true;
    flags |= 0x80u;                                             // THE STEAL
    const uint32_t mrd = g.U32(me + kDef);
    g.W8(mrd + 46u, g.U8(trd + 46u));
    g.W16(mrd + 44u, static_cast<uint16_t>(g.U16(mrd + 44u) | (1u << (g.U8(trd + 46u) & 31u))));
    g.W16(trd + 44u, static_cast<uint16_t>(g.U16(trd + 44u) & ~(1u << (g.U8(trd + 46u) & 31u))));
    g.W8(trd + 46u, 9);
    g.W8(trd + 70u, g.U8(me + kHandle));
    g.W8(mrd + 71u, g.U8(t + kHandle));
    FightStat(g, me, 0, 0);
    FightStat(g, t, 0, 1);
    const uint32_t vr = g.U32(t + kRider);
    const uint32_t drawn = g.U8(vr + 0x23Cu) >> 7;
    const int32_t vn = g.S8(vr + 0x23Eu + g.U8(vr + 0x23Au));
    const uint32_t s5 = g.U16(g.U32(Rec(g, g.U8(vr + 0x239u)) + 8u) + U(vn * 12));
    const int32_t snd = (g.U8(mrd + 1u) & 0xFu) ? 97 : 96;
    if (!c.PlaySound3D(g.S32(me + 184u), g.S32(me + 192u), snd, 0)) return false;
    g.W8(g.U32(me + kDef) + 60u, 142);
    g.W8(g.U32(t + kDef) + 60u, 32);
    uint32_t ignored = 0;
    if (!c.StanceEvent(16, g.U32(t + kRider), ((side & ~15u) ^ 0x100u) | 16u, ignored)) return false;
    if (!c.WeaponObject(s3, side & 0x100u)) return false;
    if (drawn) {
        const uint32_t vr2 = g.U32(t + kRider);
        g.W8(vr2 + 0x23Cu, static_cast<uint8_t>(g.U8(vr2 + 0x23Cu) & 0x7Fu));
        const uint32_t m = g.U32(me + kDef), v2 = g.U32(t + kDef);
        uint32_t sw = g.U8(v2 + 47u);
        if (g.U8(m + 46u) < 8u) sw = ((g.U32(m + 48u) >> ((g.U8(m + 46u) << 2) & 31u)) & 15u) + sw;
        const uint32_t sh = (g.U8(m + 46u) << 2) & 31u;
        g.W32(m + 48u, g.U32(m + 48u) & ~(15u << sh));
        g.W32(m + 48u, g.U32(m + 48u) | ((sw & 15u) << sh));
        g.W8(m + 47u, static_cast<uint8_t>(sw));
        g.W32(v2 + 48u, g.U32(v2 + 48u) & ~(15u << ((g.U8(g.U32(me + kDef) + 46u) << 2) & 31u)));
        const uint32_t slot = kObjSlots + U(172 * static_cast<int32_t>(g.S8(s3 + 0x23Bu)));
        if (!c.ObjectSound(slot, 0, 1500, 6, 0)) return false;
        g.W8(s3 + 0x23Cu, static_cast<uint8_t>(g.U8(s3 + 0x23Cu) | 0x80u));
    }
    g.W8(g.U32(t + kDef) + 47u, 0);
    ignored = g.U32(t + kDef);                                  // v0 at the call (0x800C0304), returned untouched
    if (!c.OverlayClip(s5 & 0xFFFFu, s3, (side & 0xFF00u) >> 8, (side >> 16) & 0xFFu, ignored)) return false;
    CloseQueue(g, s3);
    return !g.Faulted();
}

bool ApplyHit(GuestRam& g, Callees& c, uint32_t me, uint32_t t, uint32_t side, uint32_t flags, uint32_t& v0) { // 0x800C17B0
    const uint32_t rm = g.U32(me + kRider);
    const uint32_t hit = flags & 1u;
    uint32_t base = 0;
    const uint16_t st = HitStance(g, rm, g.U32(t + kRider), S(hit), base);
    int32_t fin = 0;
    uint32_t ret = 0;
    if (hit) {
        const uint32_t mrd = g.U32(me + kDef);
        const uint32_t k3d = g.U8(mrd + 61u);
        g.W8(mrd + 61u, static_cast<uint8_t>(((k3d & 0xFu) + 1u) & 0xFu | (k3d & 0xF0u)));
        if (!(g.U8(rm + 0x23Cu) & 0x20u)) {
            uint32_t set = g.U16(g.U32(me + kDef) + 66u);
            const uint32_t who = g.U32(t + kNode) == 0 ? g.U16(g.U32(t + kPartner) + kHandle) : g.U16(t + kHandle);
            RememberHandle(who, set);
            g.W16(g.U32(me + kDef) + 66u, static_cast<uint16_t>(set));
        }
        if (!(g.U8(g.U32(t + kRider) + 0x23Cu) & 0x20u)) {
            uint32_t set = g.U16(g.U32(t + kDef) + 64u);
            RememberHandle(g.U16(me + kHandle), set);
            g.W16(g.U32(t + kDef) + 64u, static_cast<uint16_t>(set));
        }
        if (g.S8(rm + 0x23Cu) & 0x80) base = base + (base << 1);
        if (g.S8(rm + 0x23Cu) & 0x80)
            if (!c.PlaySound3D(g.S32(me + 184u), g.S32(me + 192u), 105, 0)) return false;
        const uint32_t a2 = g.U32(me + kDef);
        const uint32_t k = g.U8(a2 + 15u) < 97u ? 96u : g.U8(a2 + 15u);
        int32_t prod = MulLo(MulLo(S(base), S(k)), S(g.U8(a2 + 12u)));
        if (prod < 0) prod += 16383;
        fin = prod >> 14;
        const uint32_t trd = g.U32(t + kDef);
        const uint32_t hp = g.U8(trd + 15u);
        const int32_t nh = S(hp) - fin;
        const int32_t q = MipsDiv(fin + 1, g.S32(kDivTab + 4u * g.U32(Gs(g) + 60u)));
        if (nh < 0) {
            const int32_t s3 = (g.U8(a2 + 1u) & 0xFu) ? 97 : 96;
            g.W8(trd + 15u, 0);                                 // 0x800C19C0
            if (IsPlayer(g, me) || g.U32(me + kNode) == 0) {
                const int32_t h = g.U16(me + kHandle);
                if (!c.QueueListenerSound(me, 109, 4, h)) return false;
                if (!c.QueueListenerSound(me, s3, 10, h)) return false;
            }
        } else {
            uint8_t b44 = g.U8(trd + 68u);
            if ((hp >= 64u && nh < 64) || (hp >= 32u && nh < 32)) b44 |= 0x20u;
            g.W8(trd + 68u, b44);
            g.W8(g.U32(t + kDef) + 15u, static_cast<uint8_t>(nh)); // 0x800C1A58
        }
        int32_t s0 = S(g.U8(g.U32(t + kDef) + 14u)) - q;
        if (g.U8(g.U32(t + kRider) + 0x23Cu) & 0x20u) {
            const int32_t pb = g.U8(g.U32(g.U32(t + kPartner) + kDef) + 14u);
            if (!(pb < s0)) s0 = pb;
        }
        const uint32_t trd2 = g.U32(t + kDef);
        if (s0 <= 0) g.W8(trd2 + 14u, 0);
        else {
            const uint32_t b = g.U8(trd2 + 14u);
            uint8_t b44 = g.U8(trd2 + 68u);
            if ((b >= 64u && s0 < 64) || (b >= 32u && s0 < 32)) b44 |= 0x20u;
            g.W8(trd2 + 68u, b44);
            g.W8(g.U32(t + kDef) + 14u, static_cast<uint8_t>(s0));
        }
        FightStat(g, me, 2, 0);
        FightStat(g, t, 2, 1);
    }
    if (g.U8(g.U32(t + kDef) + 15u) == 0) {
        if (!(g.U32(t + kFlagsC) & 0x3ECu) && !(g.U32(t + kFlagsA) & 0x20000000u)) {
            if (!KnockOff(g, c, t, g.U8(g.U32(me + kDef) + 60u), S((side >> 8) & 1u), me)) return false;
            FightStat(g, me, 1, 0);
            FightStat(g, t, 1, 1);
        }
        if (g.U32(t + kPartner) != me) {
            const uint32_t d = g.U32(t + kNode) == 0 ? g.U32(g.U32(t + kPartner) + kDef) : g.U32(t + kDef);
            int32_t x = S(g.U8(d + 37u)) - S(g.U8(d + 36u) >> 4);
            if (x < 0) x = 0;
            g.W8(d + 37u, static_cast<uint8_t>(x));
        }
    }
    NoteHit(g, me, t);                                          // 0x800C1C18
    if (g.U32(g.U32(t + kRider) + 0x25Cu) < 2u) {
        const uint32_t s6 = side & 0xFFFFFFF0u;
        if (hit) {
            if (!(g.U32(t + kFlagsC) & 0x7FFu)) {
                if (g.U32(t + kNode) != 0) {
                    uint32_t rec = g.U8(rm + 0x239u);
                    if (g.U8(rm + 0x23Cu) & 0x20u) rec -= 20u;
                    HitShove(g, t, g.U8(rm + 0x23Au), S(rec), S((s6 >> 8) & 1u));
                }
                const bool who = IsPlayer(g, t) ||
                                 (g.U32(t + kNode) == 0 && !(g.U8(g.U32(t + kRider) + 0x23Cu) & 0x40u));
                if (who && g.U32(kAttractFlag) == 0)
                    if (!HitRumble(g, c, t, fin)) return false;
            }
            const uint32_t an = g.U32(rm + 0x21Cu);
            g.W32(an + 36u, g.U32(an + 36u) | 0x80u);           // 0x800C1D10
        }
        uint32_t ignored = 0;
        ret = 1;
        if (!c.StanceEvent(st, g.U32(t + kRider), (s6 ^ 0x100u) | 0x10u, ignored)) return false;
    }
    if (hit) {
        const bool who = IsPlayer(g, me) ||
                         (g.U32(me + kNode) == 0 && !(g.U8(g.U32(me + kRider) + 0x23Cu) & 0x40u));
        if (who && g.U32(kAttractFlag) == 0)
            if (!HitRumble(g, c, me, fin)) return false;
    }
    v0 = ret;
    return !g.Faulted();
}

bool FightUpdate(GuestRam& g, Callees& c, uint32_t me, uint16_t slot, uint32_t sp) { // 0x800C035C
    const uint32_t r = g.U32(me + kRider);                      // s3
    uint32_t t = slot == kNoTarget ? 0u : g.U32(kPool0Ptr) + 1096u * slot; // s0
    uint32_t flags = 0;                                         // sp+24
    uint32_t step = 0;                                          // sp+28
    bool started = false;                                       // s8
    if (g.U8(g.U32(me + kDef) + 60u) & 0x40u) flags = 32;
    if (!Gated(g, me, r)) {
        if ((g.U16(r + 0x220u) - 30u) < 8u) {
            uint32_t ignored = 0;
            return FightEnd(g, c, r, ignored);                  // 0x800C0430
        }
        if (Category(g, g.U16(r + 0x220u)) == 3) {
            if (!FightStep(g, c, me, step)) return false;
        } else {
            if (!FightStart(g, c, me, t)) return false;
            started = true;
        }
        if (slot == kNoTarget) return true;
    }
    const int32_t along = Project(g, me + 504u, t + 528u, t + 504u); // s6
    int32_t s4;
    if (g.U8(Gs(g) + 57u) == 3 && IsPlayer(g, t)) s4 = 5;
    else s4 = CanEngage(g, me, t, S(0u - U(along)));
    int32_t lat;                                                // s2
    const uint32_t mr = g.U32(me + 0x168u);
    if (mr == g.U32(t + 0x168u) && ((mr >> 16) == 0 || g.U32(me + 0x150u) == g.U32(t + 0x150u))) {
        lat = S(g.U32(me + 344u) - g.U32(t + 344u));
        if (g.S32(t + 364u) < 0) lat = S(0u - U(lat));
    } else {
        lat = Project(g, me + 184u, t + 432u, t + 184u);
    }
    uint32_t s5 = 0;
    if ((g.U8(g.U32(t + kRider) + 0x23Cu) & 0x10u) && me != g.U32(t + kPartner) &&
        g.U32(g.U32(g.U32(t + kPartner) + kRider) + 0x25Cu) < 2u)
        s5 = lat > 0 ? 1u : 0u;
    s5 <<= 1;
    if (Gated(g, me, r)) {
        if (Category(g, g.U16(r + 0x220u)) == 3)
            if (!FightStep(g, c, me, step)) return false;
        if (!(U(s4) & 4u)) {
            // FightContinue's command record lives at its entry sp - 40 + 16; bytes 4..7 (sp - 92 here)
            // are the word CanEngage / FightStep saved there from s1, which is `me` (their `sw s1,20(sp)`
            // at the same depth). AiPushCommand's mode-2 rotation copies them, so they are put back.
            g.W32(sp - 92u, me);
            int32_t cont = 0;
            if (!FightContinue(g, c, me, t, along, U(s4) | s5, sp - 72u, cont)) return false;
            if (cont == 0) return true;
        }
        if (!(g.U32(me + kFlagsC) & 0x600u) && (g.U32(me + kFlagsA) & kBit27))
            if (!FightSteer(g, c, me, t, along, sp - 72u)) return false;
        if ((g.U16(r + 0x220u) - 30u) < 8u) return true;
    }
    if (s4 == 0) return true;
    if (U(s4) & 4u) {
        const bool riding = g.S32(t + 576u) >= 6553 && g.S32(g.U32(t + kRider) + 0x25Cu) == 1 &&
                            g.S32(g.U32(g.U32(t + kPartner) + kRider) + 0x25Cu) == 1;
        if (!riding && !(0xEFFFF < Iabs(along))) {              // the phase-3 bust
            g.W32(me + 924u, 0);
            g.W32(me + kFlagsB, g.U32(me + kFlagsB) | 0x80000u);
            g.W8(g.U32(t + kDef) + 39u, 254);
            if (g.S32(g.U32(t + kDef) + 40u) == 0) {
                if (g.U32(g.U32(t + kRider) + 0x25Cu) < 2u)
                    if (!c.EndRace(t, 9)) return false;
                g.W32(g.U32(t + kDef) + 40u, g.U32(Gs(g) + 16u));
            }
            g.W32(t + 924u, 0);
            g.W32(t + kFlagsB, g.U32(t + kFlagsB) | 0x80000u);
            return !g.Faulted();
        }
    }
    if (s5) lat = S(U(lat) - (g.U32(t + 304u) + g.U32(g.U32(t + kPartner) + 304u)));
    if (S(U(g.S32(0x80052F74u)) << 1) < Iabs(lat)) return true;
    if (0xB333 < Iabs(along)) return true;
    const uint32_t sideBit = (lat > 0 ? 1u : 0u) << 8;          // s4 from here
    if (s5) t = g.U32(t + kPartner);
    if (Gated(g, me, r) && Category(g, g.U16(r + 0x220u)) != 3) {
        if (!FightRestart(g, c, me, t, sideBit)) return false;
        started = true;
    }
    if (started && g.U8(g.U32(me + kDef) + 47u) != 0 && (g.U8(g.U32(me + kDef) + 60u) & 0x80u) &&
        g.S8(r + 0x23Bu) != -1) {
        flags |= 0x100u;
        if (ReachTest(g, me, t, lat, along, flags)) {
            const uint32_t rd = g.U32(me + kDef);
            g.W8(rd + 47u, static_cast<uint8_t>(g.U8(rd + 47u) - 1u));
            const uint32_t rd2 = g.U32(me + kDef);
            const uint32_t sh = (g.U8(rd2 + 46u) << 2) & 31u;
            g.W32(rd2 + 48u, g.U32(rd2 + 48u) & ~(15u << sh));
            const uint32_t rd3 = g.U32(me + kDef);
            const uint32_t sh3 = (g.U8(rd3 + 46u) << 2) & 31u;
            g.W32(rd3 + 48u, g.U32(rd3 + 48u) | ((g.U8(rd3 + 47u) & 15u) << sh3));
        }
        flags &= 0xFFFFFEFFu;
    }
    if (g.U8(g.U32(me + kDef) + 60u) == 32)
        if (!WeaponSteal(g, c, me, t, sideBit, flags)) return false;
    if (step != 0 && ReachTest(g, me, t, lat, along, flags)) {
        uint32_t ignored = 0;
        if (!ApplyHit(g, c, me, t, sideBit, flags, ignored)) return false;
    }
    return !g.Faulted();
}

// ============================================================================ the stance layer's children

bool ReleaseRiderObject(GuestRam& g, Callees& c, uint32_t r) { // 0x80095AEC
    const uint32_t bit = 1u << (U(static_cast<int32_t>(g.S8(r + 0x23Bu))) & 31u);
    const uint32_t bits = g.U32(kObjPoolBits);
    if (!(bits & bit)) return true;
    g.W32(kObjPoolBits, bits & ~bit);
    const uint32_t slot = kObjSlots + U(172 * static_cast<int32_t>(g.S8(r + 0x23Bu)));
    if (!c.ObjectRelease(r, slot, 0)) return false;
    const uint32_t o = g.U32(r + 556u);
    if (o != 0) {
        g.W32(o + 36u, 0);
        g.W32(kAnimPool + 8u, g.U32(kAnimPool + 8u) - 1u);
    }
    if (g.S8(r + 0x23Cu) & 0x80) {
        const uint32_t slot2 = kObjSlots + U(172 * static_cast<int32_t>(g.S8(r + 0x23Bu)));
        if (!c.ObjectStop(slot2)) return false;
        g.W8(r + 0x23Cu, static_cast<uint8_t>(g.U8(r + 0x23Cu) & 0x7Fu));
    }
    g.W32(r + 556u, 0);
    g.W8(r + 0x23Bu, 0xFF);
    return !g.Faulted();
}

bool CombatLeave(GuestRam& g, Callees& c, uint32_t r, uint32_t cur, uint32_t ev, uint32_t p, uint32_t& v0) { // 0x800BFD24
    (void)cur;
    (void)p;
    if (Category(g, ev) == 3) { v0 = 3; return true; }
    if (!ReleaseRiderObject(g, c, r)) return false;
    return FightEnd(g, c, r, v0);
}

bool CombatEnter(GuestRam& g, Callees& c, uint32_t r, uint32_t cur, uint32_t ev, uint32_t p, uint32_t& v0) { // 0x800BFC5C
    auto armed = [&]() { return (g.U8(g.U32(g.U32(r + 0x254u) + kDef) + 60u) & 0x80u) != 0; };
    if (Category(g, cur) != 3) {
        if (!armed()) { v0 = 0; return true; }
        if (!c.WeaponObject(r, p & 0x100u)) return false;
    }
    if (!armed()) { v0 = 0; return true; }
    v0 = 0x80;                                                  // `andi v0,v0,0x80` at 0x800BFCEC, left for the callee
    return c.OverlayClip(ev & 0xFFFFu, r, (p >> 8) & 0xFFu, (p >> 16) & 0xFFu, v0);
}

} // namespace rr::sim::fight
