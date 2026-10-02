// The opponent AI. See ai.h for the specification of every function and where each one sits in the
// three AI passes.
#include "game/sim/ai.h"

#include <cstddef>

#include "game/sim/fixed.h"
#include "game/sim/vec.h"

namespace rr::sim {
namespace {

// The original's own byte/halfword/word accessors over a raw guest block. Spelled out here because
// the port must never assume an alignment the console does not have.
inline uint32_t Ld32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
inline void St32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}
inline uint16_t Ld16(const uint8_t* p) {
    return static_cast<uint16_t>(static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8));
}
inline void St16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

// The R3000's `div`: it defines every corner case and raises no exception (src\interp\r3000.cpp
// 0x1A). `lo` is what the game reads back.
inline int32_t MipsDivLo(int32_t n, int32_t d) {
    if (d == 0) return (n >= 0) ? -1 : 1;
    if (static_cast<uint32_t>(n) == 0x80000000u && d == -1) return INT32_MIN;
    return n / d;
}

// `mult` / `mflo`: the low 32 bits of a signed 64-bit product.
inline int32_t MulLo(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b));
}
// `mult` / `mfhi`.
inline int32_t MulHi(int32_t a, int32_t b) {
    const int64_t p = static_cast<int64_t>(a) * static_cast<int64_t>(b);
    return static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(p) >> 32));
}
// The MIPS absolute-value idiom `sra/addu/xor`, which wraps on INT32_MIN exactly as the original.
inline int32_t MipsAbs(int32_t v) {
    const int32_t s = v >> 31;
    return static_cast<int32_t>((static_cast<uint32_t>(s) + static_cast<uint32_t>(v)) ^
                                static_cast<uint32_t>(s));
}
inline uint32_t Sub32(uint32_t a, uint32_t b) { return a - b; }

// `mult` followed by the `(lo >> 16) | (hi << 16)` extraction the whole engine is written in: bits
// 47..16 of a signed 64-bit product, truncated to 32.
inline uint32_t Mul64Hi16(int32_t a, int32_t b) {
    const int64_t prod = static_cast<int64_t>(a) * static_cast<int64_t>(b);
    return static_cast<uint32_t>(static_cast<uint64_t>(prod) >> 16);
}

} // namespace

// ---------------------------------------------------------------------------- SLUS 0x8001E100
void MemSet32(uint8_t* dst, uint32_t byteValue, uint32_t length) {
    // The original builds the word out of the FULL argument, not a masked byte: `sll v0,a1,8` and
    // friends keep whatever is above bit 7. Reproduce that exactly.
    const uint32_t w = byteValue | (byteValue << 8) | (byteValue << 16) | (byteValue << 24);
    if (length == 0) return;
    uint32_t left = length;
    uint8_t* p = dst;
    do {
        St32(p, w);
        left -= 4;
        p += 4;
    } while (left != 0);
}

// ---------------------------------------------------------------------------- RASHCDG 0x800B6AAC
int32_t AiProject(const int32_t p[3], const int16_t axis[3], const int32_t org[3]) {
    uint32_t acc = 0;
    for (int k = 0; k < 3; ++k) {
        const int32_t a = static_cast<int32_t>(static_cast<uint32_t>(static_cast<int32_t>(axis[k]))
                                               << 4);
        const int32_t d = static_cast<int32_t>(static_cast<uint32_t>(p[k]) -
                                               static_cast<uint32_t>(org[k]));
        acc += Mul64Hi16(a, d);
    }
    return static_cast<int32_t>(acc);
}

// ---------------------------------------------------------------------------- RASHCDG 0x800A8C48
int32_t AiHandleInList(uint32_t handle, uint32_t list) {
    if (list == 0) return 0;
    const uint32_t want = (handle & 0xFFFFu) + 1u;
    for (;;) {
        if ((list & 0x1Fu) == want) return 1;
        list >>= 5;
        if (list == 0) return 0;
    }
}

// ---------------------------------------------------------------------------- RASHCDG 0x800BD34C
void AiNibbleAdd(uint8_t* p, int32_t delta) {
    const uint32_t old = *p;
    const int32_t x = static_cast<int32_t>((old & 0xFu) + static_cast<uint32_t>(delta));
    // max(x, 0) + min(15 - x, 0), branch-free and 32-bit-wrapping exactly as the original.
    const int32_t lo = static_cast<int32_t>(~static_cast<uint32_t>(x >> 31) &
                                            static_cast<uint32_t>(x));
    const int32_t rest = static_cast<int32_t>(15u - static_cast<uint32_t>(x));
    const int32_t hi = static_cast<int32_t>(static_cast<uint32_t>(rest >> 31) &
                                            static_cast<uint32_t>(rest));
    const uint32_t v = static_cast<uint32_t>(lo) + static_cast<uint32_t>(hi);
    *p = static_cast<uint8_t>((old & 0xF0u) | v);
}

// ---------------------------------------------------------------------------- RASHCDG 0x800BCEEC
void AiNibbleDecay(uint8_t* p, int32_t step) {
    const uint32_t old = *p;
    uint32_t cur = old & 0xFu;
    const uint32_t rest = old >> 4;
    const int32_t diff = static_cast<int32_t>(cur - rest);
    const int32_t sign = diff >> 31;
    const int32_t mag = static_cast<int32_t>(
        (static_cast<uint32_t>(sign) + static_cast<uint32_t>(diff)) ^ static_cast<uint32_t>(sign));
    if (mag < step) {
        cur = rest;
    } else {
        if (diff >= 0) cur = cur - static_cast<uint32_t>(step);
        else cur = cur + static_cast<uint32_t>(step);
        const int32_t x = static_cast<int32_t>(cur);
        const int32_t lo = static_cast<int32_t>(~static_cast<uint32_t>(x >> 31) &
                                                static_cast<uint32_t>(x));
        const int32_t r = static_cast<int32_t>(15u - static_cast<uint32_t>(x));
        const int32_t hi = static_cast<int32_t>(static_cast<uint32_t>(r >> 31) &
                                                static_cast<uint32_t>(r));
        cur = static_cast<uint32_t>(lo) + static_cast<uint32_t>(hi);
    }
    *p = static_cast<uint8_t>((cur & 0xFu) | (rest << 4));
}

// ---------------------------------------------------------------------------- RASHCDG 0x800BCD10
void AiClearCommands(uint8_t* e) {
    const int32_t depth = static_cast<int8_t>(e[ent::kAiCmdDepth]);
    MemSet32(e + 0x3BC, 0, static_cast<uint32_t>(depth) << 3);
    e[ent::kAiCmdDepth] = 0;
}

// ---------------------------------------------------------------------------- SLUS 0x8002F0F4
int32_t SumSquares(const int32_t v[3]) {
    uint32_t acc = 0;
    for (int k = 0; k < 3; ++k) acc += Mul64Hi16(v[k], v[k]);
    return static_cast<int32_t>(acc);
}

// ---------------------------------------------------------------------------- SLUS 0x8004CF74
int32_t SqrtGte(int32_t x, const int16_t* table) {
    // GTE LZCR: the number of leading bits of LZCS equal to its sign bit, 1..32.
    const uint32_t u = static_cast<uint32_t>(x);
    const uint32_t probe = (x < 0) ? ~u : u;
    int32_t lz = 32;
    for (int i = 31; i >= 0; --i) {
        if ((probe >> i) & 1u) { lz = 31 - i; break; }
    }
    if (lz == 32) return 0;
    const int32_t e = lz & ~1;
    const int32_t sh = (19 - e) >> 1;
    int32_t n;
    const int32_t up = e - 24;
    if (up >= 0) n = static_cast<int32_t>(static_cast<uint32_t>(x) << (up & 31));
    else n = x >> ((24 - e) & 31);
    const int32_t index = n - 64;
    const int32_t w = table[index];
    if (sh >= 0) return static_cast<int32_t>(static_cast<uint32_t>(w) << (sh & 31));
    return static_cast<int32_t>(static_cast<uint32_t>(w) >> ((-sh) & 31));
}

// ---------------------------------------------------------------------------- SLUS 0x8002E548
int32_t Length3(const int32_t v[3], const int16_t* sqrtTable) {
    const int32_t r = SqrtGte(SumSquares(v), sqrtTable);
    return static_cast<int32_t>(static_cast<uint32_t>(r) << 2);
}

// ---------------------------------------------------------------------------- RASHCDG 0x80093CAC
void SetAimDelta(EntityView e, const int32_t* target, const int16_t* dir, const int32_t* scalar,
                 int32_t mode, const int16_t* sqrtTable) {
    uint8_t* b = e.bytes();
    if (target != nullptr) {
        for (int k = 0; k < 3; ++k) {
            const uint32_t d = static_cast<uint32_t>(target[k]) -
                               Ld32(b + ent::kAimPoint + 4u * static_cast<uint32_t>(k));
            St32(b + ent::kAimDelta + 4u * static_cast<uint32_t>(k), d);
        }
    } else {
        if (dir == nullptr) return;
        if (scalar == nullptr) return;
        int32_t out[3];
        Scale(*scalar, dir, out);
        for (int k = 0; k < 3; ++k)
            St32(b + ent::kAimDelta + 4u * static_cast<uint32_t>(k),
                 static_cast<uint32_t>(out[k]));
    }
    if (static_cast<int16_t>(Ld16(b + ent::kAimSuppress)) > 0) return;

    int32_t len;
    if (scalar != nullptr) {
        const int32_t s = *scalar;
        const int32_t sign = s >> 31;
        len = static_cast<int32_t>((static_cast<uint32_t>(sign) + static_cast<uint32_t>(s)) ^
                                   static_cast<uint32_t>(sign));
    } else {
        int32_t d[3];
        for (int k = 0; k < 3; ++k)
            d[k] = static_cast<int32_t>(Ld32(b + ent::kAimDelta + 4u * static_cast<uint32_t>(k)));
        len = Length3(d, sqrtTable);
    }
    if (len < 131) return;

    const uint32_t k = (mode != 0) ? 0x00010000u : 0x000A0000u;
    uint32_t rate;
    if (len > 0) rate = FixDiv(k, static_cast<uint32_t>(len));
    else rate = 0u - FixDiv(k, 0u - static_cast<uint32_t>(len));
    St32(b + ent::kAimRate, rate);

    uint32_t t;
    if (len > 0) t = FixDiv(static_cast<uint32_t>(len), k) + 0x8000u;
    else t = 0x8000u - FixDiv(0u - static_cast<uint32_t>(len), k);
    if (0x007FFF00 < static_cast<int32_t>(t)) t = 0x007FFF00u;
    const int32_t packed = static_cast<int32_t>(t << 8) >> 16;
    St16(b + ent::kAimSuppress, static_cast<uint16_t>(packed));
    if (packed == 0) St32(b + ent::kAimBlend, 0);
}

// ---------------------------------------------------------------------------- RASHCDG 0x800BCA68
int32_t AiPushCommand(uint8_t* cmd, int32_t mode, uint8_t* e, const AiPushEnv& env) {
    if (!(static_cast<int8_t>(e[ent::kAiCmdDepth]) < 16)) {
        e[ent::kAiCmdDepth] = 1;
        MemSet32(e + 0x3C4, 0, 120);
    }
    const int32_t depth0 = static_cast<int8_t>(e[ent::kAiCmdDepth]);
    uint8_t* slot = nullptr; // where the new command goes, when we get to the push

    // The stack index is a SIGNED byte and the original bounds it nowhere, so the offset must be
    // computed as a signed difference: `8u * (uint32_t)index` would wrap to a huge positive value
    // on a 64-bit host instead of stepping backwards as the R3000's 32-bit `addu` does.
    auto slotAt = [e](int32_t index) {
        return e + static_cast<std::ptrdiff_t>(ent::kAiCmdStack) +
               8 * static_cast<std::ptrdiff_t>(index);
    };

    if (depth0 == 0) {
        slot = e + 0x3BC; // = slot[1]
    } else if (mode == 1) {
        uint8_t* top = slotAt(depth0);
        if (Ld16(top + kAiCmdOpcode) == Ld16(cmd + kAiCmdOpcode) &&
            Ld16(top + kAiCmdTarget) == Ld16(cmd + kAiCmdTarget))
            return 1;
        slot = top + 8;
    } else if (mode == 2) {
        int32_t k = static_cast<int32_t>(static_cast<int8_t>(static_cast<uint8_t>(depth0)));
        bool found = false;
        for (;;) {
            uint8_t* s = slotAt(k);
            if (Ld16(s + kAiCmdOpcode) == Ld16(cmd + kAiCmdOpcode) &&
                Ld16(s + kAiCmdTarget) == Ld16(cmd + kAiCmdTarget)) {
                found = true;
                break;
            }
            k = static_cast<int8_t>(static_cast<uint8_t>(k - 1));
            if (k <= 0) break;
        }
        if (found) {
            // The original re-reads the index as a signed byte and the depth from the entity.
            const int32_t at = static_cast<int8_t>(static_cast<uint8_t>(k));
            if (at <= 0) {
                slot = slotAt(static_cast<int8_t>(e[ent::kAiCmdDepth])) + 8;
            } else {
                const int32_t depth = static_cast<int8_t>(e[ent::kAiCmdDepth]);
                if (at == depth) return 1;
                St16(cmd + kAiCmdStamp, Ld16(slotAt(at) + kAiCmdStamp));
                uint8_t* p = slotAt(at);
                int32_t i = at;
                for (;;) {
                    i = static_cast<int8_t>(static_cast<uint8_t>(i + 1));
                    St32(p + 0, Ld32(p + 8));
                    St32(p + 4, Ld32(p + 12));
                    const int32_t d = static_cast<int8_t>(e[ent::kAiCmdDepth]);
                    p += 8;
                    if (!(i < d)) break;
                }
                St32(p + 0, Ld32(cmd + 0));
                St32(p + 4, Ld32(cmd + 4));
                return 1;
            }
        } else {
            slot = slotAt(static_cast<int8_t>(e[ent::kAiCmdDepth])) + 8;
        }
    } else {
        slot = slotAt(static_cast<int8_t>(e[ent::kAiCmdDepth])) + 8;
    }

    // ---- push
    e[ent::kAiCmdDepth] = static_cast<uint8_t>(e[ent::kAiCmdDepth] + 1u);
    St32(slot + 0, Ld32(cmd + 0));
    St32(slot + 4, Ld32(cmd + 4));
    St16(slot + kAiCmdSpare, 0);
    St16(slot + kAiCmdStamp, 0);
    const uint32_t stamp =
        ((static_cast<uint32_t>(env.raceClock) * 2180u) + 0x8000u) >> 16;
    St16(slot + kAiCmdStamp, static_cast<uint16_t>(stamp | 0xC000u));

    if ((Ld32(e + ent::kFlagsA) & 0x08000000u) == 0) return 1;
    const uint8_t* rider = env.rider;
    const uint16_t kind = Ld16(rider + ent::kAltKind);
    if (Ld16(env.altKindTable + 8u * kind + 2u) != 3) return 1;
    const uint8_t id = rider[0x239];
    const uint16_t event = Ld16(env.fightRecords + 12u * static_cast<uint32_t>(id));
    env.stance->PlayIdleStance(event, env.riderAddress);
    return 1;
}

// ---------------------------------------------------------------------------- RASHCDG 0x800BA7F4
void AiCmdRace(EntityView e, const AiCmdRaceEnv& env) {
    uint8_t* b = e.bytes();
    const uint8_t* gs = env.gameState;
    uint8_t* riderDef = env.riderDef;

    int32_t a = 0;
    const int32_t raceClock = static_cast<int32_t>(Ld32(gs + 0x10));
    bool useHandle = raceClock < 900;
    if (!useHandle) useHandle = (riderDef[0] & 1) != 0;
    if (useHandle) {
        a = static_cast<int32_t>(Ld16(b + ent::kHandle)) - 1;
        if (a < 0) a = 0;
    }
    const uint32_t flags = env.flags;
    bool force = (flags & 1u) != 0;
    if (!force && (riderDef[1] & 0xFu) == 2 && (flags & 4u) != 0) force = true;
    if (!force && (flags & 1u) == 0) {
        // The `gameState[0x39] >= 4` arm. `sltiu v0,v0,4` is UNSIGNED, and the byte is a u8, so
        // this is simply "the Jailbreak phase has reached 4".
        if (!(gs[0x39] < 4)) {
            const uint32_t h = Ld16(b + ent::kHandle);
            if ((((h >> 1) - 1u) & 1u) != 0) force = true;
        }
    }
    if (force) a = 1;
    if ((a & 1) == 0) {
        St16(b + ent::kAimSuppress, 0);
        riderDef[0] = static_cast<uint8_t>(riderDef[0] & 0xFBu);
        return;
    }

    // Three shapes, and only the last one takes the sign test.
    bool inner = false;
    if ((gs[4] & 1u) != 0) {
        inner = (flags & 1u) != 0;
        if (!inner && (riderDef[1] & 0xFu) == 2 && (flags & 4u) != 0) inner = true;
    }
    int32_t bias;
    if (inner) {
        const int32_t lat = static_cast<int32_t>(static_cast<int16_t>(Ld16(b + ent::kAimLateral)));
        const int32_t shifted = static_cast<int32_t>(static_cast<uint32_t>(lat) << 5);
        if (static_cast<int16_t>(Ld16(b + 0x188)) == 4)
            bias = static_cast<int32_t>(Ld32(b + 0x158) - static_cast<uint32_t>(shifted));
        else
            bias = shifted; // the original falls straight into the store, with no sign test
    } else {
        bias = (static_cast<int32_t>(Ld32(b + 0x16C)) >= 0) ? static_cast<int32_t>(0xFFFE0000u)
                                                            : static_cast<int32_t>(0x00020000u);
        // The sign flips when the slice's lateral axis and the one the bike carries at +0x368 point
        // opposite ways: `(m[0]*a[0] << 4 >> 16) + (m[2]*a[2] << 4 >> 16) < 0`.
        const int32_t t0 = static_cast<int32_t>(env.slice[1]) *
                           static_cast<int32_t>(static_cast<int16_t>(Ld16(b + ent::kAimSliceAxis)));
        const int32_t t1 = static_cast<int32_t>(env.slice[3]) *
                           static_cast<int32_t>(static_cast<int16_t>(Ld16(b + ent::kAimSliceAxis + 4)));
        const int32_t s0 = static_cast<int32_t>(static_cast<uint32_t>(t0) << 4) >> 16;
        const int32_t s1 = static_cast<int32_t>(static_cast<uint32_t>(t1) << 4) >> 16;
        if (static_cast<int32_t>(static_cast<uint32_t>(s0) + static_cast<uint32_t>(s1)) < 0)
            bias = static_cast<int32_t>(0u - static_cast<uint32_t>(bias));
    }

    int16_t axis[3];
    for (int k = 0; k < 3; ++k)
        axis[k] = static_cast<int16_t>(Ld16(b + ent::kAimSliceAxis + 2u * static_cast<uint32_t>(k)));
    SetAimDelta(e, nullptr, axis, &bias, 1, env.sqrtTable);
    St16(b + ent::kAimSuppress, 1);
    St32(b + ent::kAimBlend, 0x00010000u);
    riderDef[0] = static_cast<uint8_t>(riderDef[0] | 4u);
}

// ---------------------------------------------------------------------------- RASHCDG 0x80095BF8
int32_t AiTargetSpeed(uint8_t* e, uint8_t* riderDef, uint8_t* stats, int32_t dt,
                      const AiSpeedEnv& env) {
    const uint8_t* gs = env.gameState;

    // The top of the command stack, addressed by the SIGNED depth byte exactly as AiPushCommand
    // does (and, as there, the original bounds it nowhere).
    const uint8_t* top = e + static_cast<std::ptrdiff_t>(ent::kAiCmdStack) +
                         8 * static_cast<std::ptrdiff_t>(static_cast<int8_t>(e[ent::kAiCmdDepth]));

    // ---- 1. the reference rider (0x80095C2C..0x80095D24)
    AiBikeRef ref;
    if (static_cast<int32_t>(Ld32(gs + 0x30)) == 2) {
        const AiBikeRef p1 = env.player1;
        const AiBikeRef p2 = env.player2;
        bool p1Done = Ld32(p1.riderDef + 0x28) != 0;
        if (!p1Done) p1Done = !(p1.riderDef[0x27] < 248);
        if (p1Done) {
            ref = p2;
        } else {
            bool p2Done = Ld32(p2.riderDef + 0x28) != 0;
            if (!p2Done) p2Done = !(p2.riderDef[0x27] < 248);
            if (p2Done) {
                ref = p1;
            } else {
                const int32_t prog1 = static_cast<int32_t>(Ld32(p1.entity + 0x144));
                const int32_t prog2 = static_cast<int32_t>(Ld32(p2.entity + 0x144));
                int32_t t1 = (prog1 < prog2) ? 1 : 0;
                const int32_t d = static_cast<int32_t>(
                    Sub32(p1.riderDef[0x27], p2.riderDef[0x27]));
                const int32_t ad = MipsAbs(d);
                const int32_t half =
                    static_cast<int32_t>(static_cast<uint32_t>(ad) +
                                         (static_cast<uint32_t>(ad) >> 31)) >> 1;
                const int32_t behindPlace = env.playerSlot[t1].riderDef[0x27];
                const int32_t mine = riderDef[0x27];
                const int32_t want = static_cast<int32_t>(Sub32(static_cast<uint32_t>(behindPlace),
                                                                static_cast<uint32_t>(half)));
                t1 ^= (mine < want) ? 1 : 0;
                ref = env.playerSlot[t1 & 1];
            }
        }
    } else {
        ref = env.player1;
    }

    const int32_t cls = riderDef[1] & 0xF;
    const int32_t raceType = static_cast<int32_t>(Ld32(gs + 0x3C));
    const uint8_t gs4 = gs[4];
    const int32_t raceClock = static_cast<int32_t>(Ld32(gs + 0x10));

    // ---- 2. the cop arm (0x80095D44..0x80095EF4)
    if (cls == 2) {
        if ((e[ent::kAiState] & 0x10u) == 0) return 0; // 0x80095F70
        if (!(gs[0x39] < 4)) return env.tabCopFlat[raceType];

        const int32_t sinceRelease = static_cast<int32_t>(
            Sub32(static_cast<uint32_t>(raceClock),
                  static_cast<uint32_t>(static_cast<int32_t>(Ld16(riderDef + 0x3E)) << 8)));
        const int32_t steps = (MulHi(sinceRelease, static_cast<int32_t>(0x1B4E81B5)) >> 5) -
                              (sinceRelease >> 31) -
                              static_cast<int32_t>(env.tabCopBase[2 * raceType + 1]);
        if (steps > 0) {
            int32_t n = MipsDivLo(steps, static_cast<int32_t>(env.tabCopStep[2 * raceType + 1]));
            n += 1;
            const int32_t scaled = MulLo(n, env.tabCopMul[raceType]);
            int32_t want = MulLo(env.copScale, static_cast<int32_t>(128u - static_cast<uint32_t>(scaled)));
            if (want < 0) want += 127;
            want >>= 7;
            const int32_t cap = env.tabSpeedCap[raceType];
            if (want < cap) want = cap;
            if (want < static_cast<int32_t>(Ld32(stats + 0xE0))) St32(stats + 0xE0, static_cast<uint32_t>(want));
        }
        // 0x80095E68, the tail shared with the "steps <= 0" case
        const int32_t gap = static_cast<int32_t>(
            Sub32(Ld32(e + 0x144), Ld32(ref.entity + 0x144))) >> 12;
        const int32_t answer = static_cast<int32_t>(Ld32(stats + 0xE0));
        if (static_cast<int16_t>(Ld16(e + 0x140)) != 0 && !(gap < -63) && gap < 12)
            St16(e + 0x140, static_cast<uint16_t>(Ld16(e + 0x140) | 2u));
        if (Ld16(top + kAiCmdOpcode) != 4) return answer;
        if (gap >= 0) return answer;
        if ((Ld16(top + kAiCmdTarget) >> 5) == 0) return answer;
        return env.tabSpeedCap[raceType];
    }

    // ---- 3 and 4 (0x80095EF8..0x80095F74)
    if (gs[0x39] == 1) {
        if (env.jailbreakBike != nullptr)
            return static_cast<int32_t>(Ld32(env.jailbreakBike + ent::kSpeed));
    }
    if (gs[0x39] == 3) {
        if (cls == (env.player1.riderDef[1] & 0xF)) return 0;
    }

    // ---- 5. the racer arm (0x80095F78 onward)
    const int32_t placeGap = static_cast<int32_t>(
        Sub32(ref.riderDef[0x27], riderDef[0x27]));
    if (!(env.liveBikes < MipsAbs(placeGap))) {
        if ((gs4 & 1u) != 0 && (env.raceFlags & 1u) != 0) {
            return (env.altFlag != 0) ? env.tabAltB[raceType] : env.tabAltA[raceType];
        }
        // 0x8009601C: the think-period schedule
        const int32_t bank = (gs4 & 4u) ? 6 : ((gs4 & 1u) ? 3 : 0);
        const int32_t think = env.tabThinkA[raceType + bank];
        const int32_t threshold =
            (static_cast<int32_t>(static_cast<uint32_t>(MulLo(think, 300)) ) >> 16) + 300;
        if (raceClock < threshold) {
            riderDef[0] = static_cast<uint8_t>(riderDef[0] | 1u);
            const int32_t timer = static_cast<int32_t>(Ld32(e + ent::kCrashTimer));
            const int32_t left = static_cast<int32_t>(Sub32(static_cast<uint32_t>(timer),
                                                            static_cast<uint32_t>(dt)));
            if (timer > 0) {
                if (left > 0) {
                    St32(e + ent::kCrashTimer, static_cast<uint32_t>(left));
                } else {
                    St32(e + ent::kCrashTimer, 0);
                    St32(e + ent::kFlagsB, Ld32(e + ent::kFlagsB) & 0xFFFFFDFFu);
                }
                return static_cast<int32_t>(Ld32(stats + 0xE0));
            }
            // 0x800960E0
            const int32_t p = static_cast<int32_t>(Sub32(riderDef[0x27], 1u));
            const int32_t half =
                static_cast<int32_t>(static_cast<uint32_t>(p) + (static_cast<uint32_t>(p) >> 31)) >> 1;
            const int32_t period = env.tabThinkB[raceType + bank];
            const int32_t scaled = static_cast<int32_t>(static_cast<uint32_t>(MulLo(period, 300))) >> 16;
            if (!(MulLo(half + 1, scaled) < raceClock))
                return static_cast<int32_t>(Ld32(stats + 0xE0));
            if ((riderDef[0] & 2u) != 0) return static_cast<int32_t>(Ld32(stats + 0xE0));
            const int32_t p2 = static_cast<int32_t>(Sub32(riderDef[0x27], 1u));
            const int32_t half2 =
                static_cast<int32_t>(static_cast<uint32_t>(p2) + (static_cast<uint32_t>(p2) >> 31)) >> 1;
            const int32_t period2 = env.tabThinkB[raceType + bank];
            St32(e + ent::kFlagsB, Ld32(e + ent::kFlagsB) | 0x200u);
            St32(e + ent::kCrashTimer,
                 Sub32(static_cast<uint32_t>(think),
                       static_cast<uint32_t>(MulLo(half2, static_cast<int32_t>(
                                                              static_cast<uint32_t>(period2) << 1)))));
            riderDef[0] = static_cast<uint8_t>(riderDef[0] | 2u);
            return static_cast<int32_t>(Ld32(stats + 0xE0));
        }
    } else {
        return static_cast<int32_t>(Ld32(stats + 0xE0)); // 0x80096228
    }

    // ---- 0x8009623C: the race is under way
    riderDef[0] = static_cast<uint8_t>(riderDef[0] & 0xFEu);
    const int32_t myProgress = static_cast<int32_t>(Ld32(e + 0x144));
    if (myProgress <= 0) return 0x000A0000; // the constant left in v0 by the delay slot
    const int32_t gap =
        static_cast<int32_t>(Sub32(static_cast<uint32_t>(myProgress), Ld32(ref.entity + 0x144))) >> 12;

    if (gs4 == 44 && (riderDef[0] & 0x10u) != 0) {
        int32_t v = MulLo(riderDef[0x45], static_cast<int32_t>(Ld32(stats + 0xE0)));
        if (v < 0) v += 127;
        return v >> 7;
    }
    if ((riderDef[0] & 8u) != 0) {
        uint32_t want = Ld32(ref.entity + ent::kSpeed) + 0x000A0000u;
        if (gap > 0) want += 0xFFF40000u;
        const uint32_t nearly = Ld32(stats + 0xE0) + 0xFFFE0000u;
        uint32_t flagsB = Ld32(e + ent::kFlagsB);
        if (static_cast<int32_t>(nearly) < static_cast<int32_t>(want)) flagsB |= 0x200u;
        St32(e + ent::kFlagsB, flagsB);
        return static_cast<int32_t>(want);
    }
    if (static_cast<int16_t>(Ld16(e + 0x140)) != 0 && !(gap < -63) && gap < 12) {
        int32_t v = MulLo(riderDef[0x45], static_cast<int32_t>(Ld32(stats + 0xE0)));
        const int32_t answer = (v < 0) ? ((v + 127) >> 7) : (v >> 7);
        e[ent::kAiState] = static_cast<uint8_t>(e[ent::kAiState] | 8u);
        St16(e + 0x140, static_cast<uint16_t>(Ld16(e + 0x140) | 2u));
        return answer;
    }

    // ---- 0x80096390: the rubber band itself
    if ((e[ent::kAiState] & 8u) != 0) {
        if (*env.clockStamp != raceClock) {
            const int32_t rp = static_cast<int32_t>(Ld32(ref.entity + 0x144));
            const int32_t q = static_cast<int32_t>(
                static_cast<uint32_t>(static_cast<int32_t>(
                    static_cast<uint32_t>(rp) + 4096u) >> 12) << 2);
            *env.clockStamp = raceClock;
            *env.spreadBand = static_cast<int32_t>(Sub32(3u, static_cast<uint32_t>(
                                                                 MipsDivLo(q, env.spreadDiv))));
        }
        const int32_t band = *env.spreadBand;
        const int32_t a1 = (band < 2) ? 0 : static_cast<int32_t>(Sub32(static_cast<uint32_t>(band), 1u));
        const int32_t refPlace = ref.riderDef[0x27];
        const int32_t p = static_cast<int32_t>(Sub32(static_cast<uint32_t>(refPlace), 1u));
        int32_t a3 = static_cast<int32_t>(Sub32(
            static_cast<uint32_t>(MulHi(p, static_cast<int32_t>(0x55555556))),
            static_cast<uint32_t>(p >> 31))); // (refPlace - 1) / 3
        if (!(a3 < 5)) a3 = 4;
        int32_t span = refPlace;
        if (placeGap < 0)
            span = static_cast<int32_t>(Sub32(static_cast<uint32_t>(env.bikeCap),
                                              static_cast<uint32_t>(span)));
        const int32_t t1 = (placeGap >= 0) ? 0 : 3;
        const int32_t mag = MipsAbs(placeGap);
        const int32_t base = 4 * a3 + 20 * a1;
        const int32_t w0 = MulLo(env.profile[base + t1],
                                 static_cast<int32_t>(Sub32(static_cast<uint32_t>(mag), 1u)));
        const int32_t w1 = MulLo(env.profile[base + (t1 ^ 1)],
                                 static_cast<int32_t>(Sub32(static_cast<uint32_t>(span),
                                                            static_cast<uint32_t>(mag)) + 1u));
        const int32_t r = MipsDivLo(static_cast<int32_t>(static_cast<uint32_t>(w0) +
                                                         static_cast<uint32_t>(w1)),
                                    span);
        e[ent::kAiState] = static_cast<uint8_t>(e[ent::kAiState] & 0xF7u);
        e[ent::kRubberBand] = static_cast<uint8_t>(r);
    }

    // ---- 0x800964F0: the answer
    int32_t v = MulLo(static_cast<int32_t>(Ld32(stats + 0xE0)),
                      static_cast<int32_t>(static_cast<int8_t>(e[ent::kRubberBand])) + 128);
    if (v < 0) v += 127;
    const int32_t answer = v >> 7;
    if (static_cast<int16_t>(Ld16(e + 0x140)) != 0) return answer;
    return FixMul(answer, env.tabSpeedClass[riderDef[1] & 0xF]);
}

// ---------------------------------------------------------------------------- RASHCDG 0x800BA4CC
bool AiRunCommands(AiCommandNode* bikes, size_t count, int32_t dt, uint32_t skip, uint32_t maskB,
                   const AiRunEnv& env) {
    (void)dt; // only arms 1, 16 and 18 take it, and none of those is implemented here
    const uint8_t* gs = env.gameState;
    for (size_t i = 0; i < count; ++i) {
        const uint32_t bit = 1u << (static_cast<uint32_t>(i) & 31u);
        if ((skip & bit) != 0) continue;
        uint8_t* e = bikes[i].entity;
        uint8_t* slot = e + static_cast<std::ptrdiff_t>(ent::kAiCmdStack) +
                        8 * static_cast<std::ptrdiff_t>(static_cast<int8_t>(e[ent::kAiCmdDepth]));
        const uint32_t numPlayers = Ld32(gs + 0x30);
        const uint32_t handle = Ld16(e + ent::kHandle);
        const uint32_t op = Ld16(slot + kAiCmdOpcode);
        const uint32_t target = Ld16(slot + kAiCmdTarget);
        if (!(handle < numPlayers) && (target < numPlayers) && op >= 14 && op < 17) {
            env.attackerMask[target] = static_cast<uint16_t>(
                env.attackerMask[target] | (1u << ((handle - 1u) & 31u)));
        }
        if (op < 19) {
            const uint32_t arm = env.armTable[op];
            if (arm == env.armRace) {
                if ((maskB & bit) == 0 && (Ld32(e + ent::kFlagsA) & 0x08000000u) != 0) {
                    AiCmdRaceEnv ce;
                    ce.gameState = gs;
                    ce.riderDef = bikes[i].riderDef;
                    ce.slice = bikes[i].slice;
                    ce.flags = env.raceFlags;
                    ce.sqrtTable = env.sqrtTable;
                    AiCmdRace(EntityView(e), ce);
                }
            } else if (arm != env.armNone) {
                return false; // an arm this port does not contain - report, never guess
            }
        }
        St16(slot + kAiCmdStamp, static_cast<uint16_t>(Ld16(slot + kAiCmdStamp) & 0x7FFFu));
    }
    return true;
}

// ---------------------------------------------------------------------------- RASHCDG 0x800BD388
int32_t AiRecoverLine(EntityView e, const int32_t* slicePos, const int16_t* sliceTangent,
                      int32_t raceBank) {
    if ((e.U32(ent::kFlagsA) & 0x08000000u) == 0) return 0;         // 0x800BD3A8
    const uint32_t road = e.U32(0x184);                             // 0x800BD3B0
    if ((road & 1u) == 0) return 0;                                 // 0x800BD3BC
    if (!(0x00023FFF < static_cast<int32_t>(e.U32(ent::kSpeedCopy)))) {
        // ---- 0x800BD3D8: nearly stopped - snap the aim point onto the centre line.
        int32_t out[3] = {0, 0, 0};
        MulAdd(slicePos, sliceTangent, static_cast<int32_t>(e.U32(0x15C)), out);
        for (int k = 0; k < 3; ++k)
            e.SetU32(ent::kAimPoint + 4u * static_cast<uint32_t>(k), static_cast<uint32_t>(out[k]));
        e.SetU16(ent::kAimLateral, 0);                              // 0x800BD3F8
        e.bytes()[ent::kAiState] = static_cast<uint8_t>(e.bytes()[ent::kAiState] | 0x40u);
        return 1;
    }
    // ---- 0x800BD408: moving - only correct while the lateral error is inside one band.
    if ((road & 0x10u) == 0) return 0;
    int32_t width = static_cast<int32_t>(
        (static_cast<uint32_t>(static_cast<int32_t>(road) >> 8) & 0xFFFu) << 16);
    if (width < 0) width += 7;                                      // 0x800BD418, never taken
    const int32_t lateral = static_cast<int32_t>(e.U32(0x158));
    const int32_t mag = MipsAbs(lateral);
    const int32_t half = width >> 3;                                // 0x800BD438
    if (!(half < mag)) return 0;                                    // 0x800BD440
    if (!(mag < static_cast<int32_t>(static_cast<uint32_t>(half) + 0x00050000u))) return 0;
    int32_t slide = static_cast<int32_t>(static_cast<uint32_t>(half) + 0x00010000u);
    if (lateral < 0) slide = static_cast<int32_t>(0u - static_cast<uint32_t>(slide));
    slide = static_cast<int32_t>(static_cast<uint32_t>(slide) -
                                 (static_cast<uint32_t>(e.S16(ent::kAimLateral)) << 5));
    {
        int32_t base[3], out[3] = {0, 0, 0};
        int16_t axis[3];
        for (int k = 0; k < 3; ++k) {
            base[k] = static_cast<int32_t>(e.U32(ent::kAimPoint + 4u * static_cast<uint32_t>(k)));
            axis[k] = e.S16(ent::kAimSliceAxis + 2u * static_cast<uint32_t>(k));
        }
        MulAdd(base, axis, slide, out);
        for (int k = 0; k < 3; ++k)
            e.SetU32(ent::kAimPoint + 4u * static_cast<uint32_t>(k), static_cast<uint32_t>(out[k]));
    }
    e.SetU16(ent::kAimSuppress, 0);                                 // 0x800BD494
    e.SetU16(ent::kAimLateral,
             static_cast<uint16_t>(e.U16(ent::kAimLateral) + static_cast<uint32_t>(slide >> 5)));
    e.SetU32(ent::kCmdSpeed,
             ((static_cast<uint32_t>(raceBank) << 3) + 24u) << 16);                // 0x800BD4B8
    return 1;
}

// ---------------------------------------------------------------------------- RASHCDG 0x800BC8DC
void AiPopCommand(uint8_t* e, const AiPopEnv& env) {
    for (;;) {
        const int32_t depth = static_cast<int8_t>(e[ent::kAiCmdDepth]);
        if (depth == 0) return;                                     // 0x800BC8FC
        uint8_t* slot = e + static_cast<std::ptrdiff_t>(ent::kAiCmdStack) + 8 * depth;
        if (static_cast<uint32_t>(static_cast<uint16_t>(Ld16(slot + kAiCmdOpcode) - 10u)) < 7u)
            env.riderDef[0x3D] = static_cast<uint8_t>(env.riderDef[0x3D] & 0xF0u); // 0x800BC93C
        if ((Ld32(e + ent::kFlagsA) & 0x08000000u) != 0) {          // 0x800BC94C
            St32(e + ent::kCrashTimer, 0);
            const uint32_t kind = Ld16(env.rider + ent::kAltKind);
            if (Ld16(env.altKindTable + 8u * kind + 2u) == 3 && env.stance != nullptr) {
                const uint32_t rec = 12u * env.rider[0x239];
                env.stance->PlayIdleStance(Ld16(env.fightRecords + rec), env.riderAddress);
            }
        }
        MemSet32(slot, 0, 8);                                       // 0x800BC9AC
        const int32_t next = static_cast<int8_t>(static_cast<uint8_t>(e[ent::kAiCmdDepth]) - 1u);
        e[ent::kAiCmdDepth] = static_cast<uint8_t>(next);           // 0x800BC9D4
        slot = e + static_cast<std::ptrdiff_t>(ent::kAiCmdStack) + 8 * next;
        if (Ld16(slot + kAiCmdOpcode) != 16) return;                // 0x800BC9E0
        const uint32_t target = Ld16(slot + kAiCmdTarget);
        if (!(target < Ld32(env.gameState + 0x30))) return;         // 0x800BC9FC
        const uint8_t mode = env.gameState[4];
        if (mode == 36 || mode == 44) return;                       // 0x800BCA0C / 0x800BCA14
        const uint32_t m = env.attackerMask[target];
        if (m == 0) return;                                         // 0x800BCA30
        if ((m & (0u - m)) == m) return;                            // exactly one attacker left
        // 0x800BCA48: the original recurses on itself with the same entity. A loop is the same
        // thing here - nothing below the call reads a local of the outer frame.
    }
}

// ---------------------------------------------------------------------------- RASHCDG 0x800954A0
bool AiDrive(uint8_t* e, int32_t dt, const AiDriveEnv& env) {
    EntityView v(e);
    if ((v.U32(ent::kFlagsA) & 0x08000000u) != 0) {
        // ---- 0x800954D4: the AI's own line.
        v.SetU32(ent::kCmdSpeed, Ld32(env.stats + 0xE0));
        const int32_t look = FixMul(static_cast<int32_t>(Ld32(env.stats + 0x1A4)),
                                    static_cast<int32_t>(v.U32(ent::kSpeed)));
        const uint32_t mag = static_cast<uint32_t>(MipsAbs(static_cast<int32_t>(v.U32(0x158))));
        int32_t ahead = static_cast<int32_t>((mag << 1) + static_cast<uint32_t>(look));
        if (ahead < 0x000F0000) ahead = 0x000F0000;                 // 0x8009550C
        if (0x00500000 < ahead) ahead = 0x00500000;                 // 0x8009551C
        // `+0x16C` is the bike's direction of travel along the road; bit 7 of the rider record is
        // "ride the route backwards", and the original negates it there (0x80095548).
        const int32_t travel = static_cast<int32_t>(v.U32(0x16C));
        const int32_t dir = (env.riderDef[0] & 0x80u) != 0
                                ? static_cast<int32_t>(0u - static_cast<uint32_t>(travel))
                                : travel;
        int16_t axis[3] = {0, 0, 0};
        int32_t origin[3] = {0, 0, 0};
        if (env.road == nullptr) return false;
        if (!env.road->LookAhead(ahead, static_cast<int32_t>(v.U32(0x15C)), dir, 0x148,
                                 ent::kAimPoint, axis, origin))
            return false;
        int32_t point[3];
        for (int k = 0; k < 3; ++k)
            point[k] = static_cast<int32_t>(v.U32(ent::kAimPoint + 4u * static_cast<uint32_t>(k)));
        v.SetU16(ent::kAimLateral,
                 static_cast<uint16_t>(static_cast<int16_t>(AiProject(point, axis, origin) >> 5)));
        for (int k = 0; k < 3; ++k)
            v.SetU16(ent::kAimSliceAxis + 2u * static_cast<uint32_t>(k),
                     static_cast<uint16_t>(axis[k]));
    }
    // ---- 0x800955D4: the commanded speed.
    int32_t wanted;
    if (v.U16(ent::kHandle) < Ld32(env.gameState + 0x30)) {
        v.SetU16(0x140, static_cast<uint16_t>(v.U16(0x140) | 2u));  // 0x800955FC
        if ((v.U32(ent::kFlagsA) & 0x08000000u) == 0) return true;  // 0x80095610
        if ((env.gameState[4] & 1u) != 0) {
            // RASHCDG 0x80096818 (modes.h, PORTED) and, on its yes, 0x8008A998 / 0x80086AF8 through the
            // caller's arm; without one, refuse rather than guess.
            if (env.copArm == nullptr) return false;
            uint32_t yes = 0;
            if (!env.copArm->Fsm(e, dt, yes)) return false;           // 0x8009562C
            if (yes == 0) return true;                                  // 0x80095634
            if (!env.copArm->ViewReset(e)) return false;                // 0x80095664..0x800956A4
        }
        int32_t prod = MulLo(env.riderDef[0x45], static_cast<int32_t>(Ld32(env.stats + 0xE0)));
        if (prod < 0) prod += 127;                                  // 0x800956D0
        wanted = prod >> 7;
    } else {
        wanted = AiTargetSpeed(e, env.riderDef, env.stats, dt, env.speed);
    }
    const int32_t have = static_cast<int32_t>(v.U32(ent::kCmdSpeed));
    v.SetU32(ent::kCmdSpeed, static_cast<uint32_t>((wanted < have) ? wanted : have));
    return true;
}

// ---------------------------------------------------------------------------- RASHCDG 0x800BA304
bool AiDrivePass(AiDriveNode* bikes, size_t count, int32_t dt, uint32_t skip, uint32_t* maskB,
                 uint32_t* maskC, const AiDrivePassEnv& env) {
    for (size_t i = 0; i < count; ++i) {
        const uint32_t bit = 1u << (static_cast<uint32_t>(i) & 31u);
        if ((skip & bit) != 0) continue;                            // 0x800BA368
        uint8_t* e = bikes[i].entity;
        EntityView v(e);
        const int32_t depth = static_cast<int8_t>(e[ent::kAiCmdDepth]);
        const uint32_t op =
            Ld16(e + static_cast<std::ptrdiff_t>(ent::kAiCmdStack) + 8 * depth + kAiCmdOpcode);
        // 0x800BA390: the mask word is read, conditionally or-ed and stored back either way.
        if ((v.U32(ent::kFlagsB) & 0x200u) != 0) *maskC |= bit;

        auto drive = [&](void) -> bool {
            AiDriveEnv de;
            de.gameState = env.gameState;
            de.riderDef = bikes[i].riderDef;
            de.stats = bikes[i].stats;
            de.road = bikes[i].road;
            de.speed = env.speed;
            de.copArm = env.copArm;
            return AiDrive(e, dt, de);
        };

        if (static_cast<uint32_t>(op - 3u) < 15u) {                 // 0x800BA39C: 3 <= op < 18
            if (!drive()) return false;
            if (op < 4) continue;                                   // 0x800BA3B4
            if (AiRecoverLine(v, bikes[i].slicePos, bikes[i].sliceTangent, env.raceBank) == 0)
                continue;                                           // 0x800BA3C8
            *maskB |= bit;
            if (op < 5) continue;                                   // 0x800BA3E0
            if (Ld32(bikes[i].riderDef + 0x28) != 0) continue;      // 0x800BA3FC, a WORD
            if (!(bikes[i].riderDef[0x27] < 248)) continue;         // 0x800BA410
            AiPopEnv pe;
            pe.gameState = env.gameState;
            pe.riderDef = bikes[i].riderDef;
            pe.rider = bikes[i].rider;
            pe.riderAddress = bikes[i].riderAddress;
            pe.altKindTable = env.altKindTable;
            pe.fightRecords = env.fightRecords;
            pe.attackerMask = env.attackerMask;
            pe.stance = env.stance;
            AiPopCommand(e, pe);                                    // 0x800BA418
        } else {                                                    // 0x800BA428
            if (Ld32(bikes[i].riderDef + 0x28) == 0) continue;      // only riders who have finished
            if (static_cast<int32_t>(v.U32(ent::kSpeed)) < 132) continue;
            if ((v.U32(ent::kFlagsA) & 0x08000000u) == 0) continue;
            if (static_cast<int16_t>(v.U16(0x140)) == 0) continue;
            if (!drive()) return false;                             // 0x800BA478
        }
    }
    return true;
}

} // namespace rr::sim
