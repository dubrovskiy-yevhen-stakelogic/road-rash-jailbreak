#include "game/sim/anim.h"

// Every function below is one guest function, transcribed from our listing of RASHCDG.BIN
// (cfe43a77...) / SLUS_010.53 (67ed165a...). The comments carry the original's addresses. Loads
// and stores are made in the original's order - including the re-reads it makes after a store -
// so that aliasing (a program that overlaps its object, a jump target that walks off the program)
// behaves exactly as on the console.

namespace rr::sim {
namespace {

inline uint32_t Op(GuestRam& g, uint32_t a) {
    const uint32_t pc = g.U32(a + animf::kPc);
    return g.U32(a + animf::kProgram) + 12u * pc; // (pc*2 + pc)*4, wrapping
}
inline uint32_t SExt16(uint16_t v) { return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(v))); }
inline int32_t S(uint32_t v) { return static_cast<int32_t>(v); }

// The instruction budget of the longest bench row, divided by the 7 instructions of one turn of
// Tick's loop, with room to spare: a guest that loops longer than this never returns to its caller
// within any budget, so the port refuses instead of spinning.
constexpr uint64_t kTickSpinCap = 50'000'000u;

} // namespace

// ============================================================================ A1

// 0x8005C4EC Tick(a, dt): acc = dt + a->acc; while (!(acc < rate)) { frame++; acc -= rate; }
// sub = acc (0x8005C520), acc (0x8005C528). Signed compares (`slt`). One store to +0x10 per key in
// the original; the object's fields do not overlap, so the closed form below stores the same final
// value. A non-positive rate with acc >= rate spins (forever for 0); that is refused past the cap.
uint32_t AnimMachine::Tick(uint32_t a, int32_t dt) {
    const uint32_t acc0 = static_cast<uint32_t>(dt) + g_.U32(a + animf::kAcc); // 0x8005C4F4 addu
    const int32_t rate = g_.S32(a + animf::kRate);
    int32_t acc = S(acc0);
    if (!(acc < rate)) {
        if (rate > 0) {
            // acc >= rate > 0: every subtraction stays >= 0, so n = acc / rate turns, acc % rate left.
            const uint32_t n = static_cast<uint32_t>(acc / rate);
            g_.W32(a + animf::kFrame, g_.U32(a + animf::kFrame) + n);
            acc = acc % rate;
        } else {
            uint32_t frame = g_.U32(a + animf::kFrame);
            uint64_t turns = 0;
            do {
                acc = S(static_cast<uint32_t>(acc) - static_cast<uint32_t>(rate));
                frame += 1u;
                if (++turns > kTickSpinCap) {
                    refused_ = true;
                    return 1;
                }
            } while (!(acc < rate));
            g_.W32(a + animf::kFrame, frame);
        }
    }
    g_.W32(a + animf::kSub, static_cast<uint32_t>(acc));
    g_.W32(a + animf::kAcc, static_cast<uint32_t>(acc));
    return 1;
}

// 0x8005C418 Seq(a): from the pc, op 2 jumps to its `a`, op 4 steps (wrapping at the length), op 5
// clears flags bit 1 and stops, 0/1/3 stop there, opcodes >= 6 do nothing. The guard counts from
// the pc (a2 := a1 at 0x8005C434): `length < counter` (signed, 0x8005C4BC) clears bit 1 and gives up.
uint32_t AnimMachine::Seq(uint32_t a) {
    bool done = false;
    const uint32_t prog = g_.U32(a + animf::kProgram);  // t1
    uint32_t pc = g_.U32(a + animf::kPc);               // a1
    const int32_t len = g_.S32(a + animf::kLength);     // t0
    uint32_t counter = pc;                              // a2
    for (;;) {
        const uint32_t op = prog + 12u * pc;
        const uint8_t o = g_.U8(op + 1);
        if (o < 6) {                                    // the jump table at 0x8005B5EC
            switch (o) {
            case 2: pc = SExt16(g_.U16(op + 4)); break;                      // 0x8005C470
            case 4: pc += 1u; if (pc == static_cast<uint32_t>(len)) pc = 0; break; // 0x8005C48C
            case 5:                                                          // 0x8005C4A0
                done = true;
                g_.W32(a + animf::kFlags, g_.U32(a + animf::kFlags) & ~2u);
                break;
            default: done = true; break;                                     // 0x8005C4B4
            }
        }
        if (len < S(counter)) {                                              // 0x8005C4BC
            g_.W32(a + animf::kFlags, g_.U32(a + animf::kFlags) & ~2u);
            return pc;
        }
        counter += 1u;
        if (done) return pc;
        if (g_.Faulted()) return pc; // the view refused a load: the caller fails the call
    }
}

// 0x8005BE58 ClipDone(a).
uint32_t AnimMachine::ClipDone(uint32_t a) {
    if (!(g_.U32(a + animf::kFlags) & 2u)) return 1;                         // 0x8005BE64
    const uint32_t prog = g_.U32(a + animf::kProgram);
    const uint32_t op = prog + 12u * g_.U32(a + animf::kPc);
    const uint8_t o = g_.U8(op + 1);
    if (o == 1) {                                                            // 0x8005BEAC
        if (g_.U8(prog + 1) == 3) return 0;
        if (g_.U8(prog + 13) == 3) return 0;
        const int32_t last = g_.S16(op + 6);
        const int32_t frame = g_.S32(a + animf::kFrame);
        return (frame < last) ? 0u : 1u;                                     // slt; xori 1
    }
    if (o < 2) return 0;                                                     // 0x8005BE94
    return (o == 5) ? 1u : 0u;
}

// 0x8005BEF4 ChannelFree(a).
uint32_t AnimMachine::ChannelFree(uint32_t a) {
    const uint32_t prog = g_.U32(a + animf::kProgram);
    if (g_.U8(prog + 1) == 5) return 1;
    if (g_.U32(a + animf::kPc) == 1u && g_.U8(prog + 13) == 5) return 1;
    if (!(g_.U8(prog + 1) < 2)) return 0;
    if (g_.U8(prog + 13) == 3) return 0;
    return (g_.U32(a + animf::kPc) == 0u) ? 1u : 0u;                         // sltiu a1,v0,1
}

// 0x8005BE0C Stop(a): flags &= ~2, returns the stored value.
uint32_t AnimMachine::Stop(uint32_t a) {
    const uint32_t v = g_.U32(a + animf::kFlags) & 0xFFFFFFFDu;
    g_.W32(a + animf::kFlags, v);
    return v;
}

// 0x8005BE20 StopIfPlaying(a): -3 when bit 1 is clear (nothing stored), else Stop's value.
uint32_t AnimMachine::StopIfPlaying(uint32_t a) {
    const uint32_t v1 = g_.U32(a + animf::kFlags);
    if (!(v1 & 2u)) return 0xFFFFFFFDu;
    const uint32_t v = v1 & 0xFFFFFFFDu;
    g_.W32(a + animf::kFlags, v);
    return v;
}

// 0x8005BE44 Resume(a): acc := 0 AFTER the flags load, flags |= 2.
uint32_t AnimMachine::Resume(uint32_t a) {
    const uint32_t v = g_.U32(a + animf::kFlags);
    g_.W32(a + animf::kAcc, 0);
    g_.W32(a + animf::kFlags, v | 2u);
    return v | 2u;
}

// 0x8005C338 QuatGet(a, out): the root part's channels 3..6 (`key` at a+0x80, +0x98, +0xB0, +0xC8).
uint32_t AnimMachine::QuatGet(uint32_t a, uint32_t out) {
    uint16_t v = 0;
    for (uint32_t k = 0; k < 4; ++k) {
        v = g_.U16(a + 128u + 24u * k);
        g_.W16(out + 2u * k, v);
    }
    return v;
}

// 0x8005C36C QuatSet(a, in).
uint32_t AnimMachine::QuatSet(uint32_t a, uint32_t in) {
    uint16_t v = 0;
    for (uint32_t k = 0; k < 4; ++k) {
        v = g_.U16(in + 2u * k);
        g_.W16(a + 128u + 24u * k, v);
    }
    return v;
}

// SLUS 0x80012858 BankSwitch(a, bank): owner read FIRST; bank; T.sampled := 0 (not -1, which forces
// the next ClipSelect to decode); owner byte +9 |= 4; the program re-read, op 0's opcode := 5.
uint32_t AnimMachine::BankSwitch(uint32_t a, uint32_t bank) {
    const uint32_t owner = g_.U32(a + animf::kOwner);
    g_.W32(a + animf::kBank, bank);
    g_.W16(a + animf::kTrack, 0);
    g_.W8(owner + 9u, static_cast<uint8_t>(g_.U8(owner + 9u) | 4u));
    const uint32_t prog = g_.U32(a + animf::kProgram);
    g_.W8(prog + 1u, 5);
    return 5;
}

// RASHCDG 0x80068D20 SeatRelease(bike, rider, idx): when the seat record `bike + 8 idx + 0x38`
// holds `rider`, clear the rider's byte +0x48, the record's two words and the rider's parent +0x34.
// Returns the seat word it read.
uint32_t AnimMachine::SeatRelease(uint32_t bike, uint32_t rider, uint32_t idx) {
    const uint32_t seat = bike + (idx << 3);
    const uint32_t v = g_.U32(seat + 56u);
    if (v != rider) return v;
    g_.W8(v + 72u, 0);
    g_.W32(seat + 56u, 0);
    g_.W32(seat + 60u, 0);
    g_.W32(v + 52u, 0);
    return v;
}

// ============================================================================ A2

// 0x8005E2BC DecodeHeader(a, T): the current clip's DMD3 header into the channel table at T.
// T.sampled := -1, T.count := 3 + 4 x parts; three root channels (raw arrays
// when FLAGS bit 0, else constant 0), then the quaternion channels (coded records while at least 3
// payload bytes are left when FLAGS bit 1, else 4 x parts raw arrays). A leaf.
uint32_t AnimMachine::DecodeHeader(uint32_t a, uint32_t t) {
    uint32_t t0 = t + 4u;                                          // the channel's ptr word
    const uint32_t cp = g_.U32(a + animf::kClip);                  // t1
    uint32_t a3 = cp + 24u;                                        // the payload (not +0x14's word)
    g_.W16(t + 2u, static_cast<uint16_t>(g_.U8(cp + 15u) * 4u + 3u));
    int32_t t2 = g_.S32(cp + 4u);                                  // the block size
    const uint32_t keys2 = static_cast<uint32_t>(g_.U16(cp + 16u)) << 1; // v1
    const uint32_t npq = static_cast<uint32_t>(g_.U8(cp + 15u)) << 2;   // a0
    g_.W16(t + 0u, 0xFFFF);
    const uint8_t f13 = g_.U8(cp + 13u);
    t2 = S(static_cast<uint32_t>(t2) - 24u);
    if (f13 & 1u) {                                                // 0x8005E308: raw root channels
        uint32_t e = t + 10u;                                      // the channel's `next`
        for (int k = 0; k < 3; ++k) {
            t2 = S(static_cast<uint32_t>(t2) - keys2);
            g_.W8(e + 14u, static_cast<uint8_t>(g_.U8(e + 14u) & 0xFDu));
            g_.W32(t0, a3);
            const uint16_t x = g_.U16(a3);
            t0 += 24u;
            g_.W16(e - 2u, x);
            g_.W16(e, x);
            a3 += static_cast<uint32_t>(g_.U16(cp + 16u)) << 1;
            e += 24u;
        }
    } else {                                                       // 0x8005E360: constant 0
        uint32_t e = t + 8u;                                       // the channel's `key`
        for (int k = 0; k < 3; ++k) {
            t0 += 24u;
            const uint8_t v = g_.U8(e + 16u);
            g_.W16(e + 2u, 0);
            g_.W16(e, 0);
            g_.W8(e + 16u, static_cast<uint8_t>((v & 0xFCu) | 2u));
            e += 24u;
        }
    }
    if (g_.U8(cp + 13u) & 2u) {                                    // 0x8005E394: coded records
        if (t2 < 3) return 0x800D0000u;                            // the delay-slot `lui v0,0x800d`
        uint32_t e = t0 + 6u;                                      // a1: the channel's `next`
        for (;;) {
            uint8_t v1 = static_cast<uint8_t>(g_.U8(e + 14u) | 2u);
            g_.W8(e + 14u, v1);
            const uint32_t anim = (static_cast<uint32_t>(g_.U16(a3 + 2u)) & 0x8000u) >> 15;
            v1 = static_cast<uint8_t>(v1 & 0xFEu);
            g_.W8(e + 14u, static_cast<uint8_t>(anim | v1));
            const uint32_t w1 = g_.U16(a3 + 2u);
            const uint32_t a0 = a3 + 2u;
            const uint32_t base = w1 & 0x7FFFu;
            g_.W32(e + 2u, base);                                   // the 15-bit base
            if (base & 0x4000u) g_.W32(e + 2u, base | 0xFFFF8000u); // ... sign-extended
            if (!(g_.U8(e + 14u) & 1u)) {                          // constant: base << 6
                const uint32_t v = g_.U32(e + 2u) << 6;
                g_.W16(e - 2u, static_cast<uint16_t>(v));
                g_.W16(e, static_cast<uint16_t>(v));
            } else {                                               // animated
                const uint32_t sc = g_.U16(a0 + 2u) & 0x7FFFu;
                g_.W16(e + 10u, static_cast<uint16_t>(sc));
                const uint32_t prod = g_.U32(e + 2u) * sc;          // mult, low word
                const uint32_t w = (g_.U16(a0 + 4u) & 0x7FFFu) >> 8;
                g_.W8(e + 16u, static_cast<uint8_t>(w));
                g_.W16(e - 2u, static_cast<uint16_t>(S(prod) >> 9));
                g_.W32(t0, a3 + 6u);                                // the bit stream
                g_.W8(e + 17u, 8);                                  // bit position
                const uint8_t fl = g_.U8(e + 14u);
                const uint8_t wb = g_.U8(e + 16u);
                g_.W32(e + 6u, 0);                                  // run count
                g_.W8(e + 12u, 0);                                  // run value
                g_.W8(e + 14u, static_cast<uint8_t>(fl & 0xFBu));
                const uint32_t tab = g_.U32(kAnimWidthTable + (static_cast<uint32_t>(wb) << 2));
                const uint32_t first = g_.U8(tab);
                const uint32_t mask = (1u << (wb & 31u)) - 1u;      // sllv
                g_.W8(e + 13u, static_cast<uint8_t>((first - 1u) & mask));
            }
            const uint16_t key = g_.U16(e - 2u);                    // 0x8005E4C4
            t0 += 24u;
            g_.W16(e, key);
            const int32_t n = g_.S16(a3);
            e += 24u;
            t2 = S(static_cast<uint32_t>(t2) - (static_cast<uint32_t>(n + 1) << 1));
            a3 += (static_cast<uint32_t>(n) << 1) + 2u;
            if (t2 < 3) return 1;
            if (g_.Faulted()) return 1;
        }
    }
    // 0x8005E500: raw quaternions. v0 is still the `slti v0,t2,3` of the delay slot at 0x8005E3A4
    // when there are none to copy (parts == 0); after the loop it is the loop's final `slt`, 0.
    if (npq == 0) return (t2 < 3) ? 1u : 0u;
    uint32_t e = t0 + 6u;
    for (uint32_t k = 0; k < npq; ++k) {
        g_.W8(e + 14u, static_cast<uint8_t>(g_.U8(e + 14u) & 0xFDu));
        g_.W32(t0, a3);
        const uint16_t x = g_.U16(a3);
        t0 += 24u;
        g_.W16(e - 2u, x);
        g_.W16(e, x);
        a3 += static_cast<uint32_t>(g_.U16(cp + 16u)) << 1;
        e += 24u;
    }
    return 0;
}

// 0x8005C39C ClipSelect(a, clip): the clip block of `bank->clips[clip & 0xFF]`, header decoded -
// unless the current block's index +0x0E equals the request and T.sampled is -1. Returns the
// current clip word, re-read after the decode.
uint32_t AnimMachine::ClipSelect(uint32_t a, uint32_t clip) {
    const uint32_t cur = g_.U32(a + animf::kClip);
    if (cur != 0u && (clip & 0xFFu) == g_.U8(cur + 14u) && g_.S16(a + animf::kTrack) == -1)
        return g_.U32(a + animf::kClip);
    const uint32_t bank = g_.U32(a + animf::kBank);
    const uint32_t table = g_.U32(bank + 4u);
    const uint32_t cp = g_.U32(table + ((clip & 0xFFu) << 2));
    g_.W32(a + animf::kClip, cp);                                  // the delay slot of the jal
    DecodeHeader(a, a + animf::kTrack);
    return g_.U32(a + animf::kClip);
}

// 0x8005C52C LoopRestart(a): the looping op's clip re-selected, frame := 0, ACC := 0 (+0x1C, not the
// sub-frame), ex re-armed from the op. Returns ex.
uint32_t AnimMachine::LoopRestart(uint32_t a) {
    const uint32_t op = Op(g_, a);
    ClipSelect(a, g_.U8(op));
    g_.W32(a + animf::kFrame, 0);
    g_.W32(a + animf::kAcc, 0);
    const uint32_t v = g_.U32(op + 8u);
    g_.W32(a + animf::kEx, v);
    return v;
}

// 0x8005C58C Advance(a, dt): tick, then say whether the op at the pc is over. The op AND the clip
// are read before the tick (s1, s3).
uint32_t AnimMachine::Advance(uint32_t a, int32_t dt) {
    const uint32_t op = Op(g_, a);                                 // s1
    const uint32_t cp = g_.U32(a + animf::kClip);                  // s3
    const uint8_t o = g_.U8(op + 1u);
    if (o == 1) {                                                  // 0x8005C894: play once
        Tick(a, dt);
        if (!(g_.S32(a + animf::kFrame) < g_.S16(op + 6u))) g_.W32(a + animf::kSub, 0);
        return (g_.S16(op + 6u) < g_.S32(a + animf::kFrame)) ? 1u : 0u; // PAST the last key
    }
    if (o == 0) {                                                  // 0x8005C608: loop
        Tick(a, dt);
        const int32_t keys1 = static_cast<int32_t>(g_.U16(cp + 16u)) - 1;
        if (!(g_.S32(a + animf::kFrame) < keys1)) g_.W32(a + animf::kSub, 0);
        if (g_.S32(a + animf::kFrame) < static_cast<int32_t>(g_.U16(cp + 16u))) return 0;
        const uint32_t loops = g_.U32(a + animf::kLoops);
        if (loops != 0xFFFFu) g_.W32(a + animf::kLoops, loops - 1u); // the lh-extended -1 never matches
        if (g_.U32(a + animf::kLoops) == 0u) return 1;
        LoopRestart(a);
        return 0;
    }
    if (o == 3) {                                                  // 0x8005C678: blend
        Tick(a, dt);
        if (g_.S32(a + animf::kFrame) < g_.S16(op + 4u)) return 0;
        g_.W32(a + animf::kFrame, 0);
        g_.W32(a + animf::kSub, 0);
        if (!(g_.U8(op + 2u) & 4u)) return 1;
        // rewrite op 0 (and 1) as the new clip; the program pointer is re-read before every store
        g_.W32(a + animf::kPc, g_.U32(a + animf::kLength) - 1u);
        g_.W8(g_.U32(a + animf::kProgram) + 2u, g_.U8(op + 26u));
        g_.W32(g_.U32(a + animf::kProgram) + 8u, g_.U32(op + 32u));
        uint32_t P = 0;
        uint8_t code = 0;
        if (g_.U8(op + 0u) == 1) {                                 // the new clip plays once
            g_.W8(g_.U32(a + animf::kProgram) + 0u, g_.U8(op + 28u));
            g_.W8(g_.U32(a + animf::kProgram) + 1u, 1);
            g_.W16(g_.U32(a + animf::kProgram) + 4u, 0);
            {
                const uint16_t k1 = static_cast<uint16_t>(g_.U16(cp + 16u) - 1u); // s3: the OLD clip
                g_.W16(g_.U32(a + animf::kProgram) + 6u, k1);
            }
            g_.W32(g_.U32(a + animf::kProgram) + 8u, g_.U32(op + 32u));
            P = g_.U32(a + animf::kProgram);
            if (g_.U8(P + 2u) & 8u) g_.W8(P + 3u, g_.U8(op + 27u));
            else g_.W8(P + 3u, 0);
            P = g_.U32(a + animf::kProgram); g_.W8(P + 14u, g_.U8(P + 2u));
            P = g_.U32(a + animf::kProgram); g_.W32(P + 20u, g_.U32(P + 8u));
            P = g_.U32(a + animf::kProgram); g_.W8(P + 12u, g_.U8(P + 0u));
            P = g_.U32(a + animf::kProgram); g_.W8(P + 15u, g_.U8(P + 3u));
            code = 5;
        } else {                                                   // it loops
            g_.W8(g_.U32(a + animf::kProgram) + 0u, g_.U8(op + 28u));
            g_.W8(g_.U32(a + animf::kProgram) + 1u, 0);
            P = g_.U32(a + animf::kProgram);
            if (g_.U8(P + 2u) & 8u) g_.W8(P + 3u, g_.U8(op + 27u));
            else g_.W8(P + 3u, 0);
            g_.W16(g_.U32(a + animf::kProgram) + 4u, 0xFFFF);
            g_.W16(g_.U32(a + animf::kProgram) + 6u, 0);
            P = g_.U32(a + animf::kProgram); g_.W8(P + 14u, g_.U8(P + 2u));
            P = g_.U32(a + animf::kProgram); g_.W32(P + 20u, g_.U32(P + 8u));
            P = g_.U32(a + animf::kProgram); g_.W8(P + 12u, g_.U8(P + 0u));
            P = g_.U32(a + animf::kProgram); g_.W8(P + 15u, g_.U8(P + 3u));
            code = 2;
        }
        P = g_.U32(a + animf::kProgram);                           // 0x8005C878
        g_.W8(P + 13u, code);
        g_.W16(g_.U32(a + animf::kProgram) + 16u, 0);
        g_.W16(g_.U32(a + animf::kProgram) + 18u, 0);
        return 1;
    }
    if (o == 5) return 1;                                          // 0x8005C8D0
    return 0;                                                      // 2, 4, >= 6
}

// 0x8005C8F4 PoseStart(a): prime the op at the pc.
uint32_t AnimMachine::PoseStart(uint32_t a) {
    const uint32_t op = Op(g_, a);                                 // s0
    const uint8_t o = g_.U8(op + 1u);
    if (o == 1) {                                                  // 0x8005CA60
        const uint32_t cp = ClipSelect(a, g_.U8(op));
        uint32_t rate;
        if (g_.U8(op + 2u) & 8u) {
            rate = g_.U8(op + 3u);
            if (rate == 0) rate = 10;
        } else {
            rate = g_.U16(cp + 18u);
            if (rate == 0) rate = 10;
        }
        g_.W32(a + animf::kRate, rate);
        g_.W32(a + animf::kAcc, 0);
        g_.W32(a + animf::kEx, g_.U32(op + 8u));
        const uint32_t keys = g_.U16(cp + 16u);
        if (!(g_.S16(op + 6u) < static_cast<int32_t>(keys))) g_.W16(op + 6u, static_cast<uint16_t>(keys - 1u));
        const uint32_t v = SExt16(g_.U16(op + 4u));
        g_.W32(a + animf::kFrame, v);
        return v;
    }
    if (o == 0) {                                                  // 0x8005C9E8
        const uint32_t cp = ClipSelect(a, g_.U8(op));
        g_.W32(a + animf::kFrame, 0);
        uint32_t rate;
        if (g_.U8(op + 2u) & 8u) {
            rate = g_.U8(op + 3u);
            if (rate == 0) rate = 10;
        } else {
            rate = g_.U16(cp + 18u);
            if (rate == 0) rate = 10;
        }
        g_.W32(a + animf::kRate, rate);
        g_.W32(a + animf::kAcc, 0);
        g_.W32(a + animf::kLoops, SExt16(g_.U16(op + 4u)));      // lh: 0xFFFF -> 0xFFFFFFFF
        const uint32_t v = g_.U32(op + 8u);
        g_.W32(a + animf::kEx, v);
        return v;
    }
    if (o == 3) {                                                  // 0x8005C958
        const uint8_t b2 = g_.U8(op + 2u);
        if (!(b2 & 2u) || !(b2 >> 7)) {
            pose_.TransitionCapture(a, op, a + animf::kBlend);
            g_.W8(op + 2u, static_cast<uint8_t>(g_.U8(op + 2u) | 2u));
        }
        g_.W32(a + animf::kFrame, 0);
        g_.W32(a + animf::kEx, g_.U32(op + 8u));
        uint32_t rate;
        if (g_.U8(op + 2u) & 8u) rate = g_.U8(op + 3u);
        else rate = g_.U32(a + animf::kBlend + 4u);                // B.rate, a WORD
        if (rate == 0) rate = 10;
        g_.W32(a + animf::kRate, rate);
        return rate;
    }
    return 3;                                                      // 2, 4, 5, >= 6: nothing primed
}

// 0x8005CB04 AdvanceFrame(a, dt): Advance; when the op is over and the object still plays, pc + 1
// (stored first), wrapped to 0 at the length, then Seq, then PoseStart.
uint32_t AnimMachine::AdvanceFrame(uint32_t a, int32_t dt) {
    if (Advance(a, dt) == 0) return 0;
    if (!(g_.U32(a + animf::kFlags) & 2u)) return 0;
    const uint32_t pc = g_.U32(a + animf::kPc) + 1u;
    g_.W32(a + animf::kPc, pc);
    if (pc == g_.U32(a + animf::kLength)) g_.W32(a + animf::kPc, 0);
    g_.W32(a + animf::kPc, Seq(a));
    return PoseStart(a);
}

// 0x8005BD74 Restart(a): pc = frame = loops = acc = sub = ex = 0, rate = 10, flags &= 0xE5, pc =
// Seq(), PoseStart(), and flags bit 1 set when the program and its length are non-zero.
uint32_t AnimMachine::Restart(uint32_t a) {
    const uint32_t f = g_.U32(a + animf::kFlags);
    g_.W32(a + animf::kPc, 0);
    g_.W32(a + animf::kFrame, 0);
    g_.W32(a + animf::kLoops, 0);
    g_.W32(a + animf::kAcc, 0);
    g_.W32(a + animf::kSub, 0);
    g_.W32(a + animf::kEx, 0);
    g_.W32(a + animf::kRate, 10);
    g_.W32(a + animf::kFlags, f & 0xE5u);
    const uint32_t pc = Seq(a);
    g_.W32(a + animf::kPc, pc);
    PoseStart(a);
    if (g_.U32(a + animf::kProgram) == 0u) return 0;
    if (g_.U32(a + animf::kLength) == 0u) return 0;
    const uint32_t v = (g_.U32(a + animf::kFlags) & 0xFFFFFFFDu) | 2u;
    g_.W32(a + animf::kFlags, v);
    return v;
}

namespace {
// The second op of a start: a copy of the first, read back from what was just stored
// (0x8005BFD0..0x8005C004 and the same tail in the other two starts).
void CopyFirstOp(GuestRam& g, uint32_t p, uint8_t code) {
    const uint8_t v1 = g.U8(p + 2u);
    const uint32_t w8 = g.U32(p + 8u);
    const uint8_t b0 = g.U8(p + 0u);
    const uint8_t b3 = g.U8(p + 3u);
    g.W8(p + 13u, code);
    g.W16(p + 16u, 0);
    g.W16(p + 18u, 0);
    g.W32(p + 20u, w8);
    g.W8(p + 14u, v1);
    g.W8(p + 12u, b0);
    g.W8(p + 15u, b3);
}
} // namespace

// 0x8005BF6C HardStart(a, clip, flags, rate, [ex]): [op1 clip 0..keys-1, op5], then Restart.
uint32_t AnimMachine::HardStart(uint32_t a, uint32_t clip, uint32_t flags, uint32_t rate, uint32_t ex) {
    const uint32_t bank = g_.U32(a + animf::kBank);
    const uint32_t p = g_.U32(a + animf::kProgram);                 // t0
    const uint32_t table = g_.U32(bank + 4u);
    const uint32_t cp = g_.U32((clip << 2) + table);
    const uint32_t keys = g_.U16(cp + 16u);
    g_.W8(p + 2u, static_cast<uint8_t>(flags));
    g_.W8(p + 1u, 1);
    g_.W8(p + 0u, static_cast<uint8_t>(clip));
    g_.W16(p + 4u, 0);
    g_.W32(p + 8u, ex);
    g_.W16(p + 6u, static_cast<uint16_t>(keys - 1u));
    g_.W8(p + 3u, (flags & 8u) ? static_cast<uint8_t>(rate) : 0);
    CopyFirstOp(g_, p, 5);
    return Restart(a);
}

// 0x8005C018 RangedStart(a, clip, flags, first, [last, rate, ex]): [op1 clip first..last, op5].
uint32_t AnimMachine::RangedStart(uint32_t a, uint32_t clip, uint32_t flags, uint32_t first,
                                  uint32_t last, uint32_t rate, uint32_t ex) {
    const uint32_t p = g_.U32(a + animf::kProgram);                 // t1
    g_.W8(p + 2u, static_cast<uint8_t>(flags));
    g_.W8(p + 0u, static_cast<uint8_t>(clip));
    g_.W8(p + 1u, 1);
    g_.W16(p + 4u, static_cast<uint16_t>(first));
    g_.W16(p + 6u, static_cast<uint16_t>(last));
    g_.W32(p + 8u, ex);
    g_.W8(p + 3u, (flags & 8u) ? static_cast<uint8_t>(rate) : 0);
    CopyFirstOp(g_, p, 5);
    return Restart(a);
}

// 0x8005C0B0 LoopStart(a, clip, flags, rate, [ex]): [op0 clip, count 0xFFFF, op2 -> 0].
uint32_t AnimMachine::LoopStart(uint32_t a, uint32_t clip, uint32_t flags, uint32_t rate, uint32_t ex) {
    const uint32_t p = g_.U32(a + animf::kProgram);                 // t0
    g_.W8(p + 2u, static_cast<uint8_t>(flags));
    g_.W32(p + 8u, ex);
    g_.W8(p + 0u, static_cast<uint8_t>(clip));
    g_.W8(p + 1u, 0);
    g_.W8(p + 3u, (flags & 8u) ? static_cast<uint8_t>(rate) : 0);
    const uint8_t v1 = g_.U8(p + 2u);
    const uint32_t w8 = g_.U32(p + 8u);
    const uint8_t b0 = g_.U8(p + 0u);
    const uint8_t b3 = g_.U8(p + 3u);
    g_.W16(p + 4u, 0xFFFF);
    g_.W16(p + 6u, 0);
    g_.W8(p + 13u, 2);
    g_.W16(p + 16u, 0);
    g_.W16(p + 18u, 0);
    g_.W32(p + 20u, w8);
    g_.W8(p + 14u, v1);
    g_.W8(p + 12u, b0);
    g_.W8(p + 15u, b3);
    return Restart(a);
}

// 0x8005C140 Transition(a, clip, queued, flags, [once, b7, rate, ex]).
// Returns Restart's value when it restarts (queued == 0 at the end), else the `ex` word.
uint32_t AnimMachine::Transition(uint32_t a, uint32_t clip, uint32_t queued, uint32_t flags,
                                 uint32_t once, uint32_t b7, uint32_t rate, uint32_t ex) {
    uint32_t s6 = flags & 1u;
    uint32_t s0 = g_.U32(a + animf::kProgram);
    const uint8_t s5 = static_cast<uint8_t>(once);                  // lbu of the stack word
    const uint8_t s7 = static_cast<uint8_t>(g_.U8(s0 + 2u) & 1u);   // the old mirror
    const uint8_t s4 = g_.U8(s0 + 0u);                              // the old clip
    uint32_t s2 = queued;
    if (s2 == 1u) {                                                 // 0x8005C19C
        if (ClipDone(a) != 0u) s2 = 0;
        else if (g_.U8(s0 + 1u) == 0) g_.W8(s0 + 1u, 1);            // a loop becomes play-once
    }
    uint32_t from;
    if (s2 == 0u) {
        from = g_.U32(a + animf::kFrame);
    } else if (s2 == 1u) {
        const uint32_t bank = g_.U32(a + animf::kBank);
        const uint32_t table = g_.U32(bank + 4u);
        const uint32_t cp = g_.U32((static_cast<uint32_t>(s4) << 2) + table);
        from = SExt16(static_cast<uint16_t>(g_.U16(cp + 16u) - 1u));
        s0 += 12u;
    } else {
        from = 0xFFFFFFFFu;
        s6 = 0;
    }
    g_.W8(s0 + 2u, static_cast<uint8_t>(flags));
    if (flags & 8u) {
        g_.W8(s0 + 2u, static_cast<uint8_t>(flags | 8u));
        g_.W8(s0 + 3u, static_cast<uint8_t>(rate));
    } else {
        g_.W8(s0 + 3u, 0);
    }
    const uint8_t v1 = g_.U8(s0 + 2u);
    g_.W8(s0 + 0u, s5);
    g_.W8(s0 + 1u, 3);
    g_.W16(s0 + 4u, 5);
    g_.W16(s0 + 6u, 0);
    g_.W32(s0 + 8u, 0);
    g_.W8(s0 + 12u, 0);
    g_.W8(s0 + 14u, static_cast<uint8_t>(flags));
    g_.W8(s0 + 13u, 4);
    g_.W16(s0 + 16u, s4);
    g_.W16(s0 + 18u, static_cast<uint16_t>(from));
    g_.W32(s0 + 20u, 0);
    g_.W8(s0 + 26u, 0);
    g_.W8(s0 + 24u, 0);
    g_.W8(s0 + 2u, static_cast<uint8_t>((b7 << 5) | ((v1 & 0xDFu) | 0x80u) | 4u));
    g_.W8(s0 + 14u, static_cast<uint8_t>(s7 | (flags & 0xFEu)));
    if (flags & 8u) g_.W8(s0 + 26u, static_cast<uint8_t>(g_.U8(s0 + 26u) | 8u));
    const uint8_t b3 = g_.U8(s0 + 3u);
    const uint8_t b26 = g_.U8(s0 + 26u);
    g_.W8(s0 + 25u, 4);
    g_.W16(s0 + 28u, static_cast<uint16_t>(clip));
    g_.W16(s0 + 30u, 0);
    g_.W8(s0 + 36u, 0);
    g_.W8(s0 + 37u, 5);
    g_.W32(s0 + 32u, ex);
    g_.W8(s0 + 27u, b3);
    g_.W8(s0 + 26u, static_cast<uint8_t>(s6 | (b26 & 0xFEu)));
    if (s2 == 0u) return Restart(a);
    return ex;
}

// ============================================================================ A3

// 0x8005E1D8 AnimationPass(desc): for every object with flags bit 1 - ApplyFrame when owner byte +9
// has bit 0 or 1, or flags bit 4 is clear (flags read BEFORE); then, unless flags bit 3 (frozen,
// re-read after the pose), AdvanceFrame(dt = game_state+0x1C). The object base, the count and the
// game-state pointer are re-read every turn. Returns the count when it is <= 0, else 0.
uint32_t AnimMachine::Pass(uint32_t desc) {
    if (g_.U32(desc + 8u) == 0u) return 0;
    int32_t count = g_.S32(desc + 12u);
    if (count <= 0) return static_cast<uint32_t>(count);
    int32_t i = 0;
    uint32_t off = 0;
    do {
        const uint32_t obj = g_.U32(desc) + off;
        const uint32_t f = g_.U32(obj + animf::kFlags);
        if (f & 2u) {
            const uint32_t owner = g_.U32(obj + animf::kOwner);
            if ((g_.U8(owner + 9u) & 3u) != 0u || !(f & 0x10u)) pose_.ApplyFrame(obj);
            if (!(g_.U32(obj + animf::kFlags) & 8u)) {
                const uint32_t gs = g_.U32(kAnimGameStatePtr);
                AdvanceFrame(obj, g_.S32(gs + 28u));
            }
        }
        if (Failed()) return 0;
        count = g_.S32(desc + 12u);
        ++i;
        off += kAnimObjectBytes;
    } while (i < count);
    return 0;
}

} // namespace rr::sim
