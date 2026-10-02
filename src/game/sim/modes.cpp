#include "game/sim/modes.h"

#include "game/sim/coll_util.h"
#include "game/sim/hud.h"        // the HUD leaves the modes' elements call
#include "game/sim/road_runtime.h" // RouteFindLegView, SLUS 0x8003B4B0

// Every function below is transcribed from our own disassembly of SLUS_010.53 (SHA-1
// 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1) and RASHCDG.BIN (SHA-1
// cfe43a7786759f2cb9c57751cf99e84d1074782c). Comments give the original's addresses. Arithmetic wraps
// in uint32_t wherever the original's does.

namespace rr::sim {

namespace {

using cu::Add;
using cu::S;
using cu::Sub;
using cu::U;

constexpr uint32_t kGameStatePtr = 0x8005B2F8;
constexpr uint32_t kPlayer1Ptr   = 0x8005B38C;
constexpr uint32_t kPlayerBikes  = 0x8005B268;
constexpr uint32_t kLiveBikes    = 0x8005B1F8;
constexpr uint32_t kPoolTable    = 0x800CE4D0; // pool 0: +0 base, +4 stride, +12 -> the high index
constexpr uint32_t kPool3Table   = 0x800CE500; // pool 3, the same shape
constexpr uint32_t kViewArray    = 0x800CD898;
constexpr uint32_t kViewStride   = 1132;
constexpr uint32_t kAltKind      = 0x800541D4; // 8 bytes per stance: +2 u16 the kind
constexpr uint32_t kFightPtr     = 0x8005AD4C; // -> FIGHT.BIN's 12-byte records
constexpr uint32_t kCopSpeedTab  = 0x80053138; // s32 by bank: the cop's initial speed (18.0)
constexpr uint32_t kArrestSpdTab = 0x80053048; // s32 by bank: the arresting cop's speed
constexpr uint32_t kFieldSpdA    = 0x8005303C; // s32 by bank: a non-cop player's speed, state 1
constexpr uint32_t kFieldSpdB    = 0x80053054; //   ... started at the roadside
constexpr uint32_t kRideSpdA     = 0x80053030; // s32 by bank: the player cop behind the field
constexpr uint32_t kRideSpdB     = 0x80053048; //   ... started at the roadside
constexpr uint32_t kBehindTab    = 0x80053168; // s32 by bank: how far behind the field it waits
constexpr uint32_t kBeatTab      = 0x800530F0; // s32 by bank: the arrest cut-scene's beat (x 1/2)
constexpr uint32_t kFarGate      = 0x8005ADF0; // s8: the escape column step; +1 u8 the ride speed byte
constexpr uint32_t kRiderModels  = 0x80054114; // u8 by the rider's +0xB4

uint32_t Gs(GuestRam& g) { return g.U32(kGameStatePtr); }
uint32_t Rd(GuestRam& g, uint32_t e) { return g.U32(e + 1084u); }
uint32_t View(GuestRam& g, uint32_t e) { return kViewArray + kViewStride * g.U16(e + 172u); }
uint32_t Cls(GuestRam& g, uint32_t e) { return g.U8(Rd(g, e) + 1u) & 0xFu; }
int32_t BankTab(GuestRam& g, uint32_t tab) { return g.S32(tab + (g.U32(Gs(g) + 60u) << 2)); }

} // namespace

// ============================================================================ the leaves

void ModeStun(GuestRam& g, uint32_t t) {                               // 0x80097308
    const uint32_t r = g.U32(t + 852u);
    g.W8(Rd(g, t) + 39u, 254);
    g.W32(r + 552u, g.U32(r + 552u) & 0xFFEFF067u);
    g.W32(t + 720u, 0x20000u);
    if (!(static_cast<uint32_t>(g.U16(r + 544u) - 72u) < 2u)) {       // 0x80097340: not stance 72 / 73
        g.W32(t + 480u, 0);
        g.W32(r + 488u, 0);
        g.W32(r + 484u, 0);
        g.W32(r + 480u, 0);
        g.W32(r + 464u, 0);
        g.W32(r + 460u, 0);
        g.W32(r + 456u, 0);
    }
    g.W32(Rd(g, t) + 40u, g.U32(Gs(g) + 16u));
    g.W32(t + 924u, 0);
}

uint32_t ModeJailTest(GuestRam& g, uint32_t e, uint32_t out) {         // 0x8009DA4C
    int32_t n = g.S32(g.U32(kPool3Table + 12u));
    uint32_t o = g.U32(kPool3Table);
    int32_t best = 0x03E70000;
    while (n >= 0) {                                                   // 0x8009DA6C
        if (o != 0u && g.U16(o + 172u) != 0u && g.U32(o + 180u) == 0u) {
            const int32_t d = cu::Shl(Sub(g.S32(e + 324u), g.S32(o + 324u)), 4);
            const int32_t m = d >> 31;
            const int32_t a = Add(m, d) ^ m;
            if (a < best) {                                            // 0x8009DAB4
                g.W32(out, g.S32(o + 480u) < 131 ? 1u : 0u);
                best = a;
            }
        }
        o += g.U32(kPool3Table + 4u);
        --n;
        if (g.Faulted()) return 0;
    }
    return best == 0x03E70000 ? 0xFFFF0000u : U(best);
}

// ============================================================================ the functions with callees

bool ModeJailRelease(GuestRam& g, uint32_t e, uint32_t sp, ModeCallees& c) {
    const uint32_t f = sp - 32u;                                       // 0x800A0708
    if (!c.Remount(e, 1, f)) return false;
    const uint32_t v = U(BankTab(g, kCopSpeedTab));
    const uint32_t piece = g.U32(e + 372u);
    int32_t edge = 0;
    g.W32(e + 576u, v);
    g.W32(e + 480u, v);
    g.W32(e + 924u, v);
    if (piece != 0u) {                                                 // 0x800A0754
        edge = g.S32(piece + 80u);
        if (g.S32(e + 364u) >= 0) edge = g.S32(piece + 208u);
    }
    const int32_t lat = S(U(edge) - 0x8000u + ((0u - g.U32(e + 364u)) & 0x10000u));
    g.W32(e + 344u, U(lat));
    cu::GMulAdd(g, e + 184u, g.U32(e + 340u) + 2u, lat, e + 184u);    // SLUS 0x8002EAD8
    g.W32(e + 504u, g.U32(e + 184u));
    g.W32(e + 508u, g.U32(e + 188u));
    g.W32(e + 512u, g.U32(e + 192u));
    return !g.Faulted();
}

bool ModeArrest(GuestRam& g, uint32_t e, uint32_t t, uint32_t how, uint32_t sp, ModeCallees& c) {
    const uint32_t f = sp - 40u;                                       // 0x80096F30
    const uint32_t gs = Gs(g);
    if (!(g.U16(e + 172u) < g.U32(gs + 48u))) return !g.Faulted();     // a player
    const uint32_t rd = Rd(g, e);
    if ((g.U8(rd + 1u) & 0xFu) != 2u) return !g.Faulted();             // on a police bike
    {
        const uint32_t r = g.U32(e + 852u);
        if (!(g.U32(r + 604u) < 2u)) return !g.Faulted();              // in the saddle
        if (g.U32(r + 552u) & 0x8000u) return !g.Faulted();
    }
    if (g.U32(kModeArrestWord) != 0u) return !g.Faulted();             // the FSM idle
    if (g.U32(Rd(g, t) + 40u) != 0u) return !g.Faulted();              // the suspect still racing
    if (g.U16(t + 172u) == g.U8(gs + 6u)) {                            // 0x80096FEC: the designated one
        if (g.U8(gs + 4u) == 33u) {
            g.W8(rd + 39u, 248);
            g.W32(Rd(g, e) + 40u, g.U32(Gs(g) + 16u));
        } else {
            g.W8(rd + 39u, 252);
            if (g.Faulted()) return false;
            if (!c.SpeechCue(0, f)) return false;                      // 0x80097024
        }
    } else {
        if (g.U8(gs + 4u) == 33u) return !g.Faulted();                 // 0x8009703C
        const int32_t q = Sub(g.S32(kModeQuota), 1);
        g.W32(kModeQuota, U(q));
        if (!(q > 0)) g.W8(Rd(g, e) + 39u, 253);                       // 0x80097060: the quota is met
    }
    if (g.Faulted()) return false;
    ModeStun(g, t);                                                    // 0x80097064
    if (g.Faulted()) return false;
    if (g.U8(Gs(g) + 4u) != 33u) {                                     // 0x80097080
        uint32_t car = 0;
        if (!c.ArrestScene(e, t, f, car)) return false;                // 0x80097088
        if (car != 0u) {
            const uint32_t view = View(g, e);
            g.W32(view + 776u, 8);
            g.W32(view + 752u, car + 184u);
            g.W32(view + 756u, car + 320u);
            g.W32(view + 552u, g.U32(view + 552u) | 0x10u);
            if (g.Faulted()) return false;
            if (!c.ViewEvent(view, 12, f)) return false;               // 0x800970EC
        }
    }
    const uint32_t fl = g.U32(e + 560u) | 0x28000000u;                 // 0x800970F4
    g.W32(e + 560u, fl);
    if (fl & 0x08000000u) {
        const uint32_t r = g.U32(e + 852u);
        if (g.U16(kAltKind + 8u * g.U16(r + 544u) + 2u) == 3u) {       // 0x80097134
            const uint32_t ev = g.U16(g.U32(kFightPtr) + 12u * g.U8(r + 569u));
            if (g.Faulted()) return false;
            if (!c.StanceEvent(ev, r, 2, f)) return false;             // 0x8009715C
        }
    }
    if (g.Faulted()) return false;
    if (!c.ClearCommands(e, f)) return false;                          // 0x80097164
    const uint16_t op = (0x8F0D8 < g.S32(e + 480u)) ? 4u : 1u;         // 0x8009717C
    g.W16(f + 16u, op);
    g.W16(f + 18u, 224);
    if (g.Faulted()) return false;
    if (!c.PushCommand(f + 16u, 1, e, f)) return false;                // 0x800971A0
    g.W32(kModeArrestWord, g.U32(kModeArrestWord) | 2u);               // 0x800971B8
    if (g.U16(f + 16u) == 1u) {                                        // 0x800971C4: stopped
        g.W32(e + 720u, 0x20000u);
        g.W32(e + 484u, 0);
        g.W32(e + 576u, 0);
        g.W32(e + 480u, 0);
        g.W32(e + 924u, 0);
        g.W16(f + 20u, 0);
        g.W32(e + 464u, 0);
        g.W32(e + 460u, 0);
        g.W32(e + 456u, 0);
    } else {
        g.W32(e + 924u, U(BankTab(g, kArrestSpdTab)));                 // 0x80097218
    }
    if (static_cast<uint8_t>(g.U8(Rd(g, e) + 39u) + 4u) < 2u) {        // 0x80097238: 252 / 253
        const uint32_t view = View(g, e);
        g.W32(kModePostTail, 0);
        if (!(g.U32(view + 552u) & 0x44u)) {
            if (g.Faulted()) return false;
            if (!c.ViewEvent(view, how, f)) return false;              // 0x8009727C
        }
        const uint32_t gs2 = Gs(g);
        g.W32(Rd(g, e) + 40u, g.U32(gs2 + 16u));
        if (g.U8(Rd(g, e) + 39u) == 253u && g.U32(gs2 + 48u) == 2u) {   // 0x800972AC: two players
            const uint32_t other = g.U32(kPlayerBikes + 4u * (g.U16(e + 172u) < 1u ? 1u : 0u));
            g.W32(Rd(g, other) + 40u, g.U32(gs2 + 16u));
        }
    }
    return !g.Faulted();
}

bool ModeArrestFsm(GuestRam& g, uint32_t e, int32_t dt, uint32_t sp, ModeCallees& c, uint32_t& v0) {
    const uint32_t f = sp - 40u;                                       // 0x80096818
    v0 = 0;
    const uint32_t word = g.U32(kModeArrestWord);
    bool deflt = false;   // 0x80096DE4
    bool tail = false;    // 0x80096E18
    switch (word) {
    case 1: {                                                          // 0x80096868
        g.W32(kModeBeatClock, 0);
        if (!c.SpeechCue(4, f)) return false;
        if (Cls(g, e) != 2u) {                                         // 0x8009688C: not a cop
            g.W32(e + 924u, U(BankTab(g, g.S32(kModeRoadside) != 0 ? kFieldSpdB : kFieldSpdA)));
            return !g.Faulted();
        }
        int32_t place = 0;
        if (!c.ComputePlace(e, 0, f, place)) return false;             // 0x800968E0
        g.W8(Rd(g, e) + 39u, static_cast<uint8_t>(place));
        int32_t n = g.S32(g.U32(kPoolTable + 12u));
        uint32_t b = g.U32(kPoolTable);
        int32_t down = 0;
        while (n >= 0) {                                               // 0x80096914
            if (g.S16(b + 320u) != 0 && static_cast<uint32_t>(g.U32(g.U32(b + 852u) + 604u) - 3u) < 2u) ++down;
            b += g.U32(kPoolTable + 4u);
            --n;
            if (g.Faulted()) return false;
        }
        const int32_t lim = Sub(Sub(g.S32(kLiveBikes), down + 1), BankTab(g, kBehindTab));
        if (static_cast<int32_t>(g.U8(Rd(g, e) + 39u)) < lim) {        // 0x80096994: still ahead of the tail
            g.W32(e + 924u, U(BankTab(g, g.S32(kModeRoadside) != 0 ? kRideSpdB : kRideSpdA)));
            return !g.Faulted();
        }
        if (Cls(g, e) == 2u && g.U32(g.U32(e + 852u) + 604u) < 3u && !((g.U32(e + 36u) >> 27) & 1u)) {
            if (g.Faulted()) return false;
            if (!c.CopJoin(e, f)) return false;                        // 0x80096A24
            if (!c.SoundHold(0, f)) return false;                      // 0x80096A2C
        }
        const uint32_t leg = RouteFindLegView(g, g.U32(e + 428u), g.U16(e + 360u));   // SLUS 0x8003B4B0
        bool start = leg == 0u;
        if (!start) {
            const uint32_t sl = g.U32(e + 340u);
            int16_t x = g.S16(sl + 14u), y = g.S16(sl + 16u), z = g.S16(sl + 18u);
            if (g.S32(leg + 4u) < 0) {                                 // 0x80096A94 (y is stored back unnegated)
                x = static_cast<int16_t>(-x);
                z = static_cast<int16_t>(-z);
            }
            g.W16(f + 16u, static_cast<uint16_t>(x));
            g.W16(f + 18u, static_cast<uint16_t>(y));
            g.W16(f + 20u, static_cast<uint16_t>(z));
            const int32_t d1 = cu::GDot(g, e + 444u, f + 16u);         // SLUS 0x8002E698, three times
            const int32_t d2 = cu::GDot(g, e + 444u, f + 16u);
            const int32_t d3 = cu::GDot(g, e + 444u, f + 16u);
            const int32_t m = Add(d1 >> 31, d2) ^ (d3 >> 31);
            if (0xF332 < m) {
                start = true;                                          // 0x80096AFC
            } else {
                g.W32(e + 924u, U(BankTab(g, g.S32(kModeRoadside) != 0 ? kRideSpdB : kRideSpdA)));
                if (g.U32(kModeSkipResults) == 0u) return !g.Faulted();   // 0x80096B54
                start = true;
            }
        }
        if (start) {                                                   // 0x80096B60: the chase begins
            g.W32(kModeMissionBase, g.U32(Gs(g) + 16u));
            deflt = true;
        }
        break;
    }
    case 2: {                                                          // 0x80096B78
        if (Cls(g, e) != 2u) {
            tail = true;
            break;
        }
        const uint32_t rec = kModeJailRecords + 224u * g.U16(e + 172u);
        g.W32(rec + 116u, g.U32(Gs(g) + 16u));
        g.W32(kModeBeatClock, 0);
        g.W32(kModeJailRecords + 224u * g.U16(e + 172u) + 108u, 0);
        if (g.U16(e + 948u + 8u * U(g.S8(e + 946u))) < 3u) {         // 0x80096BF8
            deflt = true;
            break;
        }
        g.W32(kModeArrestWord, (g.U32(kModeArrestWord) & ~2u) | 4u);
        return !g.Faulted();
    }
    case 4: {                                                          // 0x80096C20
        const uint32_t rd = Rd(g, e);
        if ((g.U8(rd + 1u) & 0xFu) != 2u) {
            tail = true;
            break;
        }
        const int32_t acc = Add(g.S32(kModeBeatClock), dt);
        const bool won = static_cast<uint8_t>(g.U8(rd + 39u) + 4u) < 2u;
        g.W32(kModeBeatClock, U(acc));
        if (!(acc < 20001) && !won && !(g.U32(e + 180u) < 18u)) {      // 0x80096C58
            if (g.Faulted()) return false;
            if (!c.SpeechCue(1, f)) return false;
        }
        if (g.S32(kModeBeatClock) < (BankTab(g, kBeatTab) >> 1)) return !g.Faulted();   // 0x80096CB4
        g.W32(kModeArrestWord, g.U32(kModeArrestWord) & ~4u);
        if (won) {                                                     // 0x80096CD0
            const int32_t k = g.U8(Rd(g, e) + 39u) == 252u ? 0 : 2;
            if (g.Faulted()) return false;
            if (!c.SpeechCue(k, f)) return false;
            g.W32(kModeArrestWord, g.U32(kModeArrestWord) | 0x10u);
            return !g.Faulted();
        }
        g.W32(kModeArrestWord, g.U32(kModeArrestWord) | 8u);           // 0x80096D14
        g.W32(kModeBeatClock, 0);
        g.W32(kModeJailRecords + 224u * g.U16(e + 172u) + 116u, g.U32(Gs(g) + 16u));
        return !g.Faulted();
    }
    case 8: {                                                          // 0x80096D4C
        if (Cls(g, e) != 2u) {
            tail = true;
            break;
        }
        const int32_t acc = Add(g.S32(kModeBeatClock), dt);
        g.W32(kModeBeatClock, U(acc));
        if ((BankTab(g, kBeatTab) >> 1) < acc) {                       // 0x80096D98
            g.W32(kModeBeatClock, 0);
            g.W32(kModeArrestWord, g.U32(kModeArrestWord) & ~8u);
        }
        return !g.Faulted();
    }
    case 16:                                                           // 0x80096DC0
        if (Cls(g, e) != 2u) {
            tail = true;
            break;
        }
        return !g.Faulted();
    default:
        deflt = true;
        break;
    }
    if (deflt && !tail) {                                              // 0x80096DE4
        if (Cls(g, e) == 2u) g.W32(kModeArrestWord, g.U32(kModeArrestWord) & ~7u);
    }
    // 0x80096E18
    v0 = 1;
    if (g.U32(kModeSkipResults) != 0u) return !g.Faulted();
    if (g.U32(Rd(g, e) + 40u) != 0u) return !g.Faulted();
    const uint32_t view = View(g, e);
    if (g.U32(view + 772u) != 0u) return !g.Faulted();
    if (g.U32(view + 548u) & 0x2008u) return !g.Faulted();
    const uint32_t top = e + 956u + 8u * U(g.S8(e + 946u)) - 8u;       // 0x80096EA8
    bool swerve = false;
    if (g.U16(top) == 3u) swerve = ((g.U16(top + 2u) >> 5) ^ 3u) == 0u;
    if (0x6B4A1 < g.S32(e + 480u)) {                                   // 0x80096EDC
        if (swerve) return !g.Faulted();
        if (g.U16(e + 362u) != 0u) return !g.Faulted();
    }
    g.W32(e + 560u, g.U32(e + 560u) & 0xD7FFFFFFu);                    // the pad has the bike
    return !g.Faulted();
}

bool ModeMilestoneAdvance(GuestRam& g, uint32_t sp, ModeCallees& c, uint32_t& v0) {
    const uint32_t f = sp - 64u;                                       // 0x800C8D4C
    int32_t col = cu::Shl(g.S8(kFarGate), 16);
    v0 = 0;
    const uint32_t phase = g.U8(Gs(g) + 57u);
    switch (phase) {
    case 1: {                                                          // 0x800C8DC0
        const uint32_t p = g.U32(kPlayer1Ptr);
        g.W32(p + 704u, 0);
        g.W32(p + 564u, g.U32(p + 564u) & ~0x200u);
        const uint32_t clock = g.U32(Gs(g) + 16u);
        const bool late = g.S32(kModeTimeLimit) < S(clock);            // 0x800C8DF8
        if (late) {
            g.W32(Rd(g, p) + 40u, clock);
            g.W8(Rd(g, p) + 39u, 250);                                 // jailed
        }
        if (g.Faulted()) return false;
        if (!c.EscapeScene(late ? 0 : 1, f)) return false;             // 0x800C8E1C
        if (late) {
            const uint32_t q = g.U32(kPlayer1Ptr);
            const uint32_t view = View(g, q);
            const uint32_t car = g.U32(kModeEscapeCar);
            g.W32(view + 776u, 6);
            g.W32(view + 752u, car + 184u);
            g.W32(view + 756u, car + 320u);
            g.W32(view + 552u, g.U32(view + 552u) | 0x12u);
            if (g.Faulted()) return false;
            if (!c.ViewEvent(view, 12, f)) return false;               // 0x800C8E8C
            const uint32_t r = g.U32(kPlayer1Ptr);
            g.W32(r + 560u, g.U32(r + 560u) | 0x20000000u);
        }
        v0 = late ? 1u : 0u;
        return !g.Faulted();
    }
    case 2: {                                                          // 0x800C8EB0
        const uint32_t p = g.U32(kPlayer1Ptr);
        const uint32_t fl = g.U32(p + 560u) | 0x28000000u;
        const uint32_t fb = g.U32(p + 564u);
        g.W32(p + 704u, 0);
        g.W32(p + 924u, 0);
        g.W32(p + 560u, fl);
        g.W32(p + 564u, fb & ~0x200u);
        const uint32_t op = g.U16(p + 956u + 8u * U(g.S8(p + 946u) - 1));
        if (op == 0u) return !g.Faulted();                             // 0x800C8EFC
        if (op == 18u) {
            v0 = 1;
            return !g.Faulted();
        }
        if (g.Faulted()) return false;
        if (!c.ClearCommands(p, f)) return false;                      // 0x800C8F10
        g.W16(f + 16u, 2);
        g.W16(f + 18u, 224);
        if (!c.PushCommand(f + 16u, 0, g.U32(kPlayer1Ptr), f)) return false;   // 0x800C8F30
        g.W8(Rd(g, g.U32(kPlayer1Ptr)) + 69u, 64);
        {
            const uint32_t q = g.U32(kPlayer1Ptr);
            const uint32_t sl = g.U32(q + 340u);
            cu::GMulAdd(g, sl + 20u, sl + 14u, g.S32(q + 348u), 0x800CE5A8u);  // SLUS 0x8002EAD8
        }
        const uint32_t q = g.U32(kPlayer1Ptr);
        const uint32_t view = View(g, q);
        g.W32(view + 776u, 3);
        g.W32(view + 752u, g.U32(g.U32(q + 856u) + 852u) + 184u);
        g.W32(view + 552u, g.U32(view + 552u) | 0x12u);
        g.W32(view + 756u, g.U32(g.U32(q + 856u) + 852u) + 320u);
        if (g.Faulted()) return false;
        if (!c.ViewEvent(view, 12, f)) return false;                   // 0x800C8FD8
        return !g.Faulted();
    }
    case 3: {                                                          // 0x800C8FE8
        uint32_t p = g.U32(kPlayer1Ptr);
        g.W32(p + 560u, g.U32(p + 560u) & 0xD7FFFFFFu);
        g.W8(Rd(g, p) + 69u, g.U8(kFarGate + 1u));
        g.W32(kModePoliceOn, 1);
        const int32_t dir = g.U16(kModeMilestones + 18u) < g.U16(kModeMilestones + 14u) ? -1 : 1;
        p = g.U32(kPlayer1Ptr);
        g.W32(kModeJbDir, U(dir));
        const uint32_t cls = g.U8(Rd(g, p) + 1u) & 0xFu;
        if (dir < 0) col = cu::Neg(col);
        int32_t n = g.S32(g.U32(kPoolTable + 12u));
        uint32_t b = g.U32(kPoolTable);
        while (n >= 0) {                                               // 0x800C9090
            if ((g.U8(Rd(g, b) + 1u) & 0xFu) == cls && b != g.U32(kPlayer1Ptr)) {
                g.W32(b + 360u, g.U16(kModeMilestones + 12u));
                g.W32(b + 364u, g.U32(kModeJbDir));
                g.W32(b + 924u, 0);
                g.W32(b + 480u, 0);
                g.W32(b + 368u, (static_cast<uint32_t>(g.U16(kModeMilestones + 14u)) << 16) + U(col));
                g.W8(Rd(g, b) + 69u, g.U8(kFarGate + 1u));
                g.W32(b + 704u, 0);
                g.W32(b + 564u, g.U32(b + 564u) & ~0x200u);
                col = S(U(col) + (g.S32(kModeJbDir) >= 0 ? 0x60000u : 0xFFFA0000u));
                if (g.Faulted()) return false;
                if (!c.Remount(b, 1, f)) return false;                 // 0x800C9124
                const uint32_t r = g.U32(b + 852u);
                g.W16(b + 956u, 1);
                if (!c.RiderModel(r, g.U8(kRiderModels + g.U32(r + 180u)), f)) return false;   // 0x800C914C
                const uint32_t k = cls != 0u ? 4u : 8u;
                g.W32(r + 36u, (g.U32(r + 36u) & 0xFFFC0FFFu) | (k << 12));
            }
            b += g.U32(kPoolTable + 4u);
            --n;
            if (g.Faulted()) return false;
        }
        return !g.Faulted();
    }
    case 4: {                                                          // 0x800C9194
        g.W32(kModePoliceOn, 0);
        const uint32_t p = g.U32(kPlayer1Ptr);
        g.W32(kModeTrafficOn, 0);
        g.W32(p + 560u, g.U32(p + 560u) | 0x28000000u);
        uint32_t b = g.U32(kPoolTable);
        const uint32_t cls = g.U8(Rd(g, p) + 1u) & 0xFu;
        int32_t n = g.S32(g.U32(kPoolTable + 12u));
        while (n >= 0) {                                               // 0x800C91EC
            if ((g.U8(Rd(g, b) + 1u) & 0xFu) != cls) {
                if (g.Faulted()) return false;
                if (!c.ClearCommands(b, f)) return false;
                g.W16(f + 16u, 2);
                g.W16(f + 18u, 224);
                if (!c.PushCommand(f + 16u, 1, b, f)) return false;
            }
            b += g.U32(kPoolTable + 4u);
            --n;
            if (g.Faulted()) return false;
        }
        if (!c.ViewEvent(View(g, g.U32(kPlayer1Ptr)), 10, f)) return false;   // 0x800C9270
        return !g.Faulted();
    }
    case 5: {                                                          // 0x800C9280: ESCAPED
        const uint32_t p = g.U32(kPlayer1Ptr);
        g.W32(Rd(g, p) + 40u, g.U32(Gs(g) + 16u));
        g.W8(Rd(g, p) + 39u, 249);
        g.W32(kViewArray + 552u, g.U32(kViewArray + 552u) | 0x14u);
        return !g.Faulted();
    }
    default:
        return !g.Faulted();
    }
}

// ============================================================================ the modes' HUD elements

namespace {
constexpr uint32_t kClockArt   = 0x800D4620; // the art table's row 5, the big digits (kArtBigNum0)
constexpr uint32_t kSplitIndex = 0x8005ACD4; // s32: the next split, 0..3
constexpr uint32_t kSplitTimes = 0x800D9C40; // s32[3]: the race clock at each split, 0 = not yet
constexpr uint32_t kSplitAt    = 0x800D5F60; // s32[3]: the split distances (progress >> 12)
constexpr uint32_t kRecords    = 0x80053A88; // SLUS: 176 bytes per Time Trial race (56..64); +4k the splits
// The race clock in 1/256 s: (ticks << 8) / 300, the `mult 0x1B4E81B5` signed divide.
int32_t Seconds256(int32_t ticks) { return S(U(ticks) << 8) / 300; }
} // namespace

uint32_t HudClockDigits(GuestRam& g, int32_t t, uint32_t item, uint32_t art0) {
    if (0x176F0000 < t) return 0;                                      // SLUS 0x80013B90
    const int32_t tm = (t / 600) >> 8;
    t = Sub(t, tm * 0x25800);
    const int32_t m = (t / 60) >> 8;
    t = Sub(t, m * 0x3C00);
    const int32_t ts = (t / 10) >> 8;
    t = Sub(t, ts * 0xA00);
    const int32_t sec = t >> 8;
    int32_t tenth = S(U(Sub(t, sec * 256)) * 10u) >> 8;
    if (9 < tenth) tenth = 0;
    const int32_t d[5] = {tm, m, ts, sec, tenth};
    for (uint32_t k = 0; k < 5; ++k) HudSetArt(g, item + 36u * k, art0 + 16u * U(d[k]));
    return U(sec) & 1u;
}

void HudRaceClock(GuestRam& g, uint32_t ot, uint32_t items, uint32_t state, uint32_t dash, uint32_t bike,
                  uint32_t down, uint32_t mask) {
    const uint32_t rd = Rd(g, bike);                                   // 0x80063530
    if (g.U32(rd + 40u) != 0u) return;
    bool flash = false;
    const uint32_t gs = Gs(g);
    if (g.S8(gs) == 1 && !(g.U8(rd) & 0x40u)) {
        int32_t t;
        if (down == 0u) {
            t = Seconds256(g.S32(gs + 16u));
        } else {
            t = Seconds256(Sub(g.S32(kModeTimeLimit), g.S32(gs + 16u)));
            if (t < 0) t = 0;
            if ((t >> 8) < 31 && 0 < t) {
                flash = true;
                HudFlashTimer(g, dash + 64u);                          // SLUS 0x80013E64
            }
        }
        if (t != g.S32(state + 68u)) {
            g.W32(state + 68u, U(t));
            HudClockDigits(g, Add(t, 12), items + 1908u, kClockArt);
        }
    }
    if (mask == 0u && !(0 < g.S32(dash + 140u)) && (!flash || g.U32(dash + 80u) != 0u))
        HudLinkRange(g, ot, items, 53, 59);
}

void HudSplits(GuestRam& g, uint32_t ot, uint32_t items, uint32_t dash, uint32_t bike, uint32_t mask) {
    if (mask == 0u && !(0 < g.S32(dash + 140u))) HudLinkRange(g, ot, items, 60, 66);   // 0x800636F0
    const int32_t k = g.S32(kSplitIndex);
    if (k < 3) {
        const uint32_t at = kSplitTimes + 4u * U(k);
        if (g.U32(at) == 0u && g.S32(bike + 324u) < cu::Shl(g.S32(kSplitAt + 4u * U(k)), 12)) {
            const uint32_t gs = Gs(g);
            g.W32(dash + 180u, g.U32(gs + 16u));
            g.W32(at, g.U32(gs + 16u));
            HudClockDigits(g, Add(Seconds256(g.S32(gs + 16u)), 12), items + 2412u, kClockArt);
            const uint32_t rec = kRecords + 4u * U(g.S32(kSplitIndex)) + 176u * U(g.S32(Gs(g) + 64u) - 56);
            HudClockDigits(g, Seconds256(g.S32(rec)), items + 2664u, kClockArt);
            g.W32(kSplitIndex, U(g.S32(kSplitIndex) + 1));
        }
    }
    if (g.U32(dash + 180u) == 0u) return;                              // 0x800638A0
    HudFlashTimer(g, dash + 160u);
    if (mask != 0u) return;
    if (0 < g.S32(dash + 140u)) return;
    if (g.U32(dash + 176u) == 0u) return;
    HudLinkRange(g, ot, items, 67, 73);
    HudLinkRange(g, ot, items, 74, 80);
}

void HudCopClock(GuestRam& g, uint32_t ot, uint32_t items, uint32_t dash, uint32_t state, uint32_t bike) {
    bool flash = false;                                                // 0x80062C40
    const uint32_t gs = Gs(g);
    if (g.S8(gs) == 1 && !(g.U8(Rd(g, bike)) & 0x40u)) {
        int32_t t = Seconds256(Sub(g.S32(kModeTimeLimit), Sub(g.S32(gs + 16u), g.S32(kModeMissionBase))));
        if (t < 0) t = 0;
        if (t != g.S32(state + 68u)) {
            g.W32(state + 68u, U(t));
            HudClockDigits(g, Add(t, 12), items + 1908u, kClockArt);
        }
        if ((t >> 8) < 31) {
            flash = true;
            HudFlashTimer(g, dash + 64u);
        }
    }
    if (!flash || g.U32(dash + 80u) != 0u) HudLinkRange(g, ot, items, 53, 59);
}

bool HudSuspectName(GuestRam& g, HudCallees& c, uint32_t sp, uint32_t ot, uint32_t item, int32_t p) {
    const uint32_t f = sp - 32u;                                       // 0x8005FAC4
    (void)ot;
    const uint32_t e = g.U32(0x8005B3A0u) + 1096u * g.U8(Gs(g) + 6u);
    const int32_t id = g.U8(Rd(g, e) + 38u);
    const int16_t x = g.S16(item + 24u), y = g.S16(item + 26u);
    const uint32_t list = g.U32(0x8005B590u + 4u * U(p));
    const int32_t font = g.S32(0x8005AD2Cu);
    if (g.Faulted()) return false;
    return HudDrawText(g, c, f, font, id, x, y, list, 0x808080u) && !g.Faulted();
}

namespace {
// The heap allocation every packet writer inlines (hud.cpp's HeapTake): `bytes` from *(0x8005B470)+0x10C,
// through the heap manager SLUS 0x80021C98 when the next one would pass the end *(0x8005B4D0).
bool TakePacket(GuestRam& g, HudCallees& c, uint32_t sp, uint32_t bytes, uint32_t& at) {
    uint32_t heap = g.U32(kHudHeapPtr);
    const uint32_t next = g.U32(heap + 268u);
    if (!(next + bytes < g.U32(kHudHeapEnd))) {
        uint32_t answer = 0;
        if (!c.HeapOverflow(sp, next, bytes, &answer)) return false;
        heap = g.U32(kHudHeapPtr);
        g.W32(heap + 268u, answer);
    }
    heap = g.U32(kHudHeapPtr);
    at = g.U32(heap + 268u);
    g.W32(heap + 268u, at + bytes);
    return !g.Faulted();
}
} // namespace

bool HudArrests(GuestRam& g, HudCallees& c, uint32_t sp, uint32_t ot, uint32_t items, int32_t /*p*/, uint32_t mask) {
    const uint32_t f = sp - 88u;                                       // 0x80062610
    uint32_t gs = Gs(g);
    if ((g.U8(gs + 4u) & 0x10u) && g.S32(kHudSplitMode) == 2) {        // two players, the side by side layout
        HudLinkRange(g, ot, items, 2, 2);
        int32_t n = S(g.U8(Gs(g) + 7u) - g.U32(kModeQuota));
        const bool hi = !(n < 10);
        if (hi) {
            n -= 10;
            HudSetArt(g, items + 216u, kHudArtTable + 16u * static_cast<uint32_t>(kArtSmallNum0 + 1));
        }
        HudSetArt(g, items + 252u, kHudArtTable + 16u * U(kArtSmallNum0) + 16u * U(n));
        const int32_t first = S(g.U8(Gs(g) + 7u) - g.U32(kModeQuota)) < 10 ? 7 : 6;
        HudLinkRange(g, ot, items, first, 7);
        return !g.Faulted();
    }
    if (mask != 0u) return !g.Faulted();
    const uint32_t box = items + 108u, icon = items + 72u;             // items 3 and 2
    const uint32_t w = g.U16(box + 28u);
    const uint32_t off = static_cast<uint32_t>(g.U8(gs + 7u) >> 1) * w;
    const uint32_t quota = g.U8(gs + 7u);
    uint32_t x = g.U16(box + 24u) - off;                               // s1
    uint32_t ix = g.U16(icon + 24u) - off;                             // s4
    const uint32_t left = x, right = x + w * quota - 1u;               // sp+24, sp+28
    const uint32_t top = g.U16(box + 26u);                             // sp+16
    const uint32_t h = g.U16(box + 30u);
    const uint32_t iy = g.U16(icon + 26u);                             // sp+20
    if (quota != 0u) {
        const uint32_t bottom = (top + h - 1u) << 16;                  // s7
        int32_t k = 0;
        do {
            gs = Gs(g);
            if (k < S(g.U8(gs + 7u) - g.U32(kModeQuota))) {             // an arrest made: its badge
                uint32_t q = 0;
                if (!TakePacket(g, c, f, 20u, q)) return false;
                const int32_t semi = g.S16(icon + 34u);
                g.W32(q + 4u, (U(semi) << 25) | (semi == 0 ? 0x65000000u : 0x64000000u));
                g.W32(q + 8u, ix | (iy << 16));
                g.W32(q, g.U32(ot) | 0x04000000u);
                g.W32(q + 16u, U(static_cast<int32_t>(g.S16(icon + 16u))) | (U(static_cast<int32_t>(g.S16(icon + 18u))) << 16));
                g.W32(q + 12u, g.U8(icon + 12u) | (static_cast<uint32_t>(g.U8(icon + 13u)) << 8) |
                                   (static_cast<uint32_t>(g.U16(icon + 14u)) << 16));
                g.W32(ot, q & 0x00FFFFFFu);
            }
            uint32_t q = 0;                                            // the box's cell, a grey polyline
            if (!TakePacket(g, c, f, 32u, q)) return false;
            const uint32_t a2 = x | (top << 16);
            const uint32_t nx = x + w;
            g.W32(q + 4u, 0x4C808080u);
            g.W32(q + 16u, (nx - 1u) | bottom);
            g.W32(q + 8u, a2);
            g.W32(q + 12u, (nx - 1u) | (top << 16));
            g.W32(q + 20u, x | bottom);
            g.W32(q + 24u, a2);
            g.W32(q + 28u, 0x55555555u);
            g.W32(q, g.U32(ot) | 0x07000000u);
            g.W32(ot, q & 0x00FFFFFFu);
            x = nx;
            ++k;
            ix += w;
        } while (k < S(g.U8(Gs(g) + 7u)));
    }
    const uint32_t lx = g.U16(items + 1860u) + g.U16(items + 1864u);  // item 51: x + w
    const uint32_t ly = g.U16(items + 1862u);
    const uint32_t y1 = (ly + 1u) << 16, y4 = (ly + 4u) << 16;
    uint32_t q = 0;
    if (!TakePacket(g, c, f, 28u, q)) return false;                    // the bracket from item 51
    g.W32(q + 4u, 0x4C808080u);
    g.W32(q + 8u, (lx - 3u) | y1);
    g.W32(q + 12u, left | y1);
    g.W32(q + 16u, left | y4);
    g.W32(q + 20u, (lx - 1u) | y4);
    g.W32(q + 24u, 0x55555555u);
    g.W32(q, g.U32(ot) | 0x06000000u);
    g.W32(ot, q & 0x00FFFFFFu);
    const uint32_t rx = g.U16(items + 1824u);                          // item 50's x
    if (!TakePacket(g, c, f, 28u, q)) return false;                    // the bracket to item 50
    g.W32(q + 4u, 0x4C808080u);
    g.W32(q + 8u, (rx + 2u) | y1);
    g.W32(q + 12u, right | y1);
    g.W32(q + 16u, right | y4);
    g.W32(q + 20u, rx | y4);
    g.W32(q + 24u, 0x55555555u);
    g.W32(q, g.U32(ot) | 0x06000000u);
    g.W32(ot, q & 0x00FFFFFFu);
    return !g.Faulted();
}

bool HudArrestMessage(GuestRam& g, HudCallees& c, uint32_t sp, uint32_t items, uint32_t bike, uint32_t dash, int32_t p) {
    const uint32_t f = sp - 72u;                                       // 0x80062F34
    const uint32_t rd = Rd(g, bike);
    if ((g.U8(rd + 1u) & 0xFu) != 2u) return !g.Faulted();
    if ((g.U32(kModeArrestWord) & 0x1Eu) == 0u) return !g.Faulted();
    HudFlashTimer(g, dash + 96u);                                      // SLUS 0x80013E64
    if (g.U32(dash + 112u) == 0u) return !g.Faulted();
    const int8_t place = g.S8(Rd(g, bike) + 39u);
    int32_t id;
    if (place == -4 || place == -8) {                                  // 252 captured / 248 (race type 33)
        id = g.U8(Gs(g) + 4u) == 33u ? 0x18 : 0x12;
    } else if (place == -3) {                                          // 253 the quota met
        id = 0x14;
    } else {
        id = (g.U32(kModeArrestWord) & 8u) ? 0xF : 0x16;
    }
    if (g.U32(kHudStringTable) == 0u) return !g.Faulted();
    const int32_t w1 = g.S32(kHudMessageWidth + 4u * U(id - 14));      // GAMESTRG id + 1's width
    const int32_t w0 = g.S32(kHudMessageWidth + 4u * U(id - 15));      // id's width
    const uint32_t list = g.U32(kHudOt + 4u * U(p));
    const int32_t font = g.S32(kHudFontIndex);
    const uint32_t x0 = g.U16(items + 3732u), y0 = g.U16(items + 3734u);   // item 103
    auto s16 = [](uint32_t v) { return static_cast<int16_t>(static_cast<uint16_t>(v)); };
    if (g.Faulted()) return false;
    if (!HudDrawText(g, c, f, font, id, s16(x0 - U(w0 >> 1)), s16(y0), list, 0x808080u)) return false;
    if (!HudDrawText(g, c, f, g.S32(kHudFontIndex), id + 1, s16(x0 - U(w1 >> 1)),
                     s16(y0 + 2u * g.U16(kHudFontRecords + 24u * U(g.S32(kHudFontIndex)) + 20u)), list, 0x808080u))
        return false;
    if (id == 0xF) {
        const int32_t fnt = g.S32(kHudFontIndex);
        if (!HudDrawText(g, c, f, fnt, 0x11, s16(x0 - U(g.S32(kHudMessageWidth + 8u) >> 1)),
                         s16(y0 + U(g.S16(kHudFontRecords + 24u * U(fnt) + 20u) * 3)), list, 0x808080u))
            return false;
    }
    // the dark box behind the lines: the POLY_F4 at RASHCDG 0x800CC674, then the draw mode 0x800CCD50
    const uint32_t slot = g.U32(kHudOt + 4u * U(p));
    g.W32(0x800CC674u, g.U32(slot) | 0x05000000u);
    g.W32(slot, 0x000CC674u);
    const bool wide1 = w0 <= w1;
    const uint32_t half = U(wide1 ? (w1 >> 1) : (w0 >> 1));
    const uint32_t wmax = U(wide1 ? w1 : w0);
    const int32_t lh = g.S16(g.U32(kHudFontIndex) * 24u + kHudFontRecords + 20u);
    const uint32_t x = g.U16(items + 3732u), y = g.U16(items + 3734u);
    g.W16(0x800CC67Cu, static_cast<uint16_t>(x - half));
    g.W16(0x800CC67Eu, static_cast<uint16_t>(y - U(lh)));
    g.W16(0x800CC680u, static_cast<uint16_t>(x - half + wmax));
    g.W16(0x800CC682u, static_cast<uint16_t>(y - U(lh)));
    g.W16(0x800CC684u, static_cast<uint16_t>(x - half));
    const int32_t rows = id == 0xF ? 6 : 5;
    g.W16(0x800CC686u, static_cast<uint16_t>(y - U(lh) + U(lh * rows)));
    g.W16(0x800CC688u, static_cast<uint16_t>(x - half + wmax));
    g.W16(0x800CC68Au, static_cast<uint16_t>(y - U(lh) + U(lh * rows)));
    g.W8(0x800CCD53u, 2);                                              // SLUS 0x8004CE44(p, 1, 1, 15, NULL)
    g.W32(0x800CCD54u, 0xE1000000u | 0x200u | 0x400u | 0xFu);
    g.W32(0x800CCD58u, 0);
    const uint32_t slot2 = g.U32(kHudOt + 4u * U(p));
    g.W32(0x800CCD50u, g.U32(slot2) | 0x02000000u);
    g.W32(slot2, 0x000CCD50u);
    return !g.Faulted();
}

bool ModeMilestoneFirst(GuestRam& g, uint32_t /*sp*/) {
    const uint32_t p = g.U32(kPlayer1Ptr);                             // 0x800C92F8
    const int32_t dir = g.S32(kModeJbDir);
    if ((g.S32(p + 364u) ^ dir) < 0 && !(g.U32(p + 560u) & 0x08000000u)) {
        const int32_t d = cu::GDot(g, g.U32(p + 340u) + 14u, p + 528u); // SLUS 0x8002E698
        if (d < -46333) {                                              // 0x800C9354: turned round
            const uint32_t q = g.U32(kPlayer1Ptr);
            g.W32(q + 704u, 0);
            g.W32(q + 560u, g.U32(q + 560u) | 0x28000000u);
            g.W32(q + 564u, g.U32(q + 564u) & ~0x200u);
        }
        return !g.Faulted();
    }
    const uint32_t q = g.U32(kPlayer1Ptr);                             // 0x800C938C
    if ((g.S32(q + 364u) ^ g.S32(kModeJbDir)) < 0) return !g.Faulted();
    if (!(g.U32(q + 560u) & 0x08000000u)) return !g.Faulted();
    const int32_t lim = S((static_cast<uint32_t>(g.U16(kModeMilestones + 6u)) - 25u) << 16);
    if (!(g.S32(q + 368u) < lim)) return !g.Faulted();
    const int32_t d = cu::GDot(g, g.U32(q + 340u) + 14u, q + 528u);
    if (0xF5C2 < d) g.W32(q + 560u, g.U32(q + 560u) & 0xD7FFFFFFu);   // 0x800C940C: facing the route again
    return !g.Faulted();
}

} // namespace rr::sim
