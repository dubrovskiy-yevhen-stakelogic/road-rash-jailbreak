// The sound emitter, transcribed instruction by instruction out of our own disassembly of the
// player's own `SLUS_010.53` (SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1). Every branch, every
// shift and every truncation below is a line of that disassembly; where the original does
// something a C programmer would not, the instruction address is in the comment.
//
// Proven by `rrverify phys` rows `key_on`, `key_off`, `lookup_sound`, `alloc_voice`,
// `sound_3d_params`, `start_voice`, `play_sound_3d`, `set_listener`, `surface_sound` and
// `queue_listener_sound`: 0 mismatches over the whole of guest RAM outside the guest stack, plus
// the scratchpad, across dump-derived and randomised inputs.
#include "game/sim/sound.h"

#include "game/sim/fixed.h"

namespace rr::sim {
namespace {

// The record at 0x800D6870 and the 44-byte voice, as byte offsets.
constexpr uint32_t kSysBankCount = 0x000;
constexpr uint32_t kSysVoiceCount = 0x008;
constexpr uint32_t kSysKeyOn = 0x014;
constexpr uint32_t kSysKeyOff = 0x018;
constexpr uint32_t kSysReserved = 0x058; // s32[22], -1 = free
constexpr uint32_t kSysFreeStack = 0x0B0; // s32[24]
constexpr uint32_t kSysRing = 0x110;      // s32[25]
constexpr uint32_t kSysFreeTop = 0x174;
constexpr uint32_t kSysRingHead = 0x178;
constexpr uint32_t kSysRingTail = 0x17C;
constexpr uint32_t kSysPanDisabled = 0x184;

constexpr uint32_t kVoiceStride = 44;
constexpr uint32_t kVoiceSerial = 0x04;
constexpr uint32_t kVoiceDesc = 0x08;
constexpr uint32_t kVoiceState = 0x10;
constexpr uint32_t kVoiceChannel = 0x1C;
constexpr uint32_t kVoicePitch = 0x20;
constexpr uint32_t kVoiceVolL = 0x24;
constexpr uint32_t kVoiceVolR = 0x28;

constexpr uint32_t kListenerStride = 72;
constexpr uint32_t kListenerYaw = 0x00;
constexpr uint32_t kListenerX = 0x04;
constexpr uint32_t kListenerZ = 0x08;
constexpr uint32_t kListenerVx = 0x0C;
constexpr uint32_t kListenerVz = 0x10;
constexpr uint32_t kListenerPan = 0x14;
constexpr uint32_t kListenerQueueId = 0x30;  // and +0x34
constexpr uint32_t kListenerQueueArg = 0x38; // and +0x3C
constexpr uint32_t kListenerQueueTtl = 0x40; // and +0x44

constexpr uint32_t kGameStatePlayers = 0x30;

// `mult`/`mflo`: the LOW 32 bits of the 64-bit product, which is the same word for signed and
// unsigned operands. The original relies on that truncation; so does this.
int32_t MultLo(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b));
}
// `sra` with a shift amount the hardware masks to five bits.
int32_t Sra(int32_t v, int32_t shift) { return v >> (shift & 31); }
// `sllv`, the same masking.
uint32_t Sllv(uint32_t v, int32_t shift) { return v << (shift & 31); }

// The `sra/addu/xor` absolute value the compiler emits at 0x80019EB8 and 0x80019FE0. It wraps on
// INT32_MIN exactly as the original does, so it is written out rather than replaced with abs().
int32_t AbsWrap(int32_t v) {
    const int32_t s = v >> 31;
    return static_cast<int32_t>((static_cast<uint32_t>(s) + static_cast<uint32_t>(v)) ^
                                static_cast<uint32_t>(s));
}

// The octagonal norm of 0x80019EB8..0x80019F08, shared by the distance and the doppler speed:
//   hi = max(|a|,|b|), lo = min(|a|,|b|), m = lo + (lo>>1)
//   hi - (hi>>5) - (hi>>7) + (m>>2) + (m>>6)
// i.e. 0.9609375*hi + 0.3984375*lo in 16.16.
int32_t OctagonalNorm(int32_t a, int32_t b) {
    int32_t hi = AbsWrap(a);
    int32_t lo = AbsWrap(b);
    if (hi < lo) {
        const int32_t t = hi;
        hi = lo;
        lo = t;
    }
    const int32_t m = static_cast<int32_t>(static_cast<uint32_t>(lo) +
                                           static_cast<uint32_t>(lo >> 1));
    uint32_t v = static_cast<uint32_t>(hi) - static_cast<uint32_t>(hi >> 5);
    v -= static_cast<uint32_t>(hi >> 7);
    v += static_cast<uint32_t>(m >> 2);
    v += static_cast<uint32_t>(m >> 6);
    return static_cast<int32_t>(v);
}

void MarkOutOfWindow(bool* flag) {
    if (flag != nullptr) *flag = true;
}

} // namespace

// ---------------------------------------------------------------------------- SLUS 0x8001EB28
void SoundKeyOn(const SoundBytes& state, uint32_t mask) {
    state.SetU32(kSysKeyOn, state.U32(kSysKeyOn) | mask); // 0x8001EB38/0x8001EB40
}

// ---------------------------------------------------------------------------- SLUS 0x8001EB44
void SoundKeyOff(const SoundBytes& state, uint32_t mask) {
    state.SetU32(kSysKeyOff, state.U32(kSysKeyOff) | mask); // 0x8001EB54/0x8001EB5C
}

// ---------------------------------------------------------------------------- SLUS 0x8001E86C
uint32_t LookupSound(const SoundBankRef& bank, int32_t index, bool& ok) {
    ok = true;
    if (!bank.Holds(4, 1)) {
        ok = false;
        return 0;
    }
    const int32_t count = static_cast<int32_t>(bank.data[4]); // lbu v0,4(a0)
    if (!(index < count)) return 0;                           // slt a2,a1,v0 ; beqz
    if (index < 0) return 0;                                  // bgez a1
    const uint32_t off = 0x10u + 4u * static_cast<uint32_t>(index);
    if (!bank.Holds(off, 4)) {
        ok = false;
        return 0;
    }
    return bank.U32(off); // lw v1,16(v0) ; 0 = the bank has no sound i
}

// ---------------------------------------------------------------------------- SLUS 0x8001F9C4
int32_t AllocVoice(int32_t reserved, const SoundSystemEnv& env) {
    const SoundBytes& S = env.state;
    int32_t v = -1;                                 // li s0,-1
    const int32_t freeTop = S.S32(kSysFreeTop);     // lw v0,372(a0)
    if (freeTop >= 0) {                             // bltz v0 -> the steal arm
        const uint32_t slot = kSysFreeStack + 4u * static_cast<uint32_t>(freeTop);
        v = S.S32(slot);                            // lw s0,176(v0)
        S.SetU32(slot, 0xFFFFFFFFu);                // sw v1,176(v0)
        // The top is RE-READ before the decrement (0x8001FA04), not carried in a register.
        S.SetU32(kSysFreeTop, static_cast<uint32_t>(S.S32(kSysFreeTop) - 1));
    } else {
        const int32_t head = S.S32(kSysRingHead);
        const int32_t tail = S.S32(kSysRingTail);
        if (head != tail) {                         // beq v0,v1 -> nothing to steal
            const uint32_t slot = kSysRing + 4u * static_cast<uint32_t>(tail);
            v = S.S32(slot);
            S.SetU32(slot, 0xFFFFFFFFu);
            const int32_t next = tail + 1;
            S.SetU32(kSysRingTail, static_cast<uint32_t>(next)); // 0x8001FA50, then overwritten
            S.SetU32(kSysRingTail, static_cast<uint32_t>((next < 25) ? next : 0)); // 0x8001FA58
            SoundKeyOff(S, Sllv(1u, v));            // 0x8001FA60, `sllv a0,a0,s0`
        }
    }
    if (v == -1) return -1;                         // 0x8001FA6C - NO SOUND AT ALL

    // The voice address, built by the shift chain 2v, 3v, 12v, 11v, 44v at 0x8001FA80..0x8001FA94.
    const uint32_t voiceOff = kVoiceStride * static_cast<uint32_t>(v);

    if (reserved == 0) {                            // 0x8001FA98
        const int32_t head = S.S32(kSysRingHead);
        S.SetU32(kSysRing + 4u * static_cast<uint32_t>(head), static_cast<uint32_t>(v));
        const int32_t next = head + 1;
        S.SetU32(kSysRingHead, static_cast<uint32_t>(next));
        S.SetU32(kSysRingHead, static_cast<uint32_t>((next < 25) ? next : 0));
    } else {
        for (int i = 0; i < 22; ++i) {              // 0x8001FAA8..0x8001FAC8
            const uint32_t slot = kSysReserved + 4u * static_cast<uint32_t>(i);
            if (S.S32(slot) == -1) {
                S.SetU32(slot, static_cast<uint32_t>(v));
                break;
            }
        }
        // If all 22 are taken the voice ends up on NEITHER list - it can never be stolen and can
        // never be released. A leak in the original, reproduced here because it is the original.
    }

    uint32_t s = (*env.serial + 1u) & 0x07FFFFFFu;  // 0x8001FB08..0x8001FB14
    *env.serial = s;                                // 0x8001FB1C
    if (s == 0) {                                   // 0x8001FB20
        s = 1;
        *env.serial = 1;                            // 0x8001FB2C - the same word, written twice
    }
    env.voices.SetU32(voiceOff + kVoiceSerial, s);  // 0x8001FB38
    env.voices.SetU32(voiceOff + kVoiceState, 1);   // 0x8001FB40
    return v;
}

// ---------------------------------------------------------------------------- SLUS 0x80019E40
void Sound3DParams(int32_t p, int32_t x, int32_t z, int32_t vx2, int32_t vz2, int32_t* outVol,
                   int32_t* outPan, int32_t* outPitch, int32_t shift, const Sound3DEnv& env) {
    if (!env.listeners.valid()) { // 0x80019E80 `bnez v1` - the NULL listener base
        *outVol = 127;
        *outPan = 64;
        return;
    }
    if (env.gameState == nullptr) { // the caller must resolve *(0x8005B2F8); it is never null live
        MarkOutOfWindow(env.outOfWindow);
        return;
    }
    const uint32_t L = kListenerStride * static_cast<uint32_t>(p); // 0x80019E5C..0x80019E74
    if (static_cast<uint64_t>(kListenerStride) * static_cast<uint32_t>(p) + kListenerStride > env.listeners.size()) {
        MarkOutOfWindow(env.outOfWindow);
        return;
    }
    const SoundBytes& li = env.listeners;

    *outPan = li.S32(L + kListenerPan); // 0x80019EA4 - the default pan, before anything else

    const int32_t dx = static_cast<int32_t>(static_cast<uint32_t>(x) -
                                            static_cast<uint32_t>(li.S32(L + kListenerX)));
    const int32_t dz = static_cast<int32_t>(static_cast<uint32_t>(z) -
                                            static_cast<uint32_t>(li.S32(L + kListenerZ)));
    const int32_t d = Sra(OctagonalNorm(dx, dz), shift); // `srav` at 0x80019F14

    if (0x00400000 < d) { // 64.0 world units: OUT OF RANGE (0x80019F1C, a SIGNED compare)
        *outVol = 0;
        if (outPitch == nullptr) return;
        *outPitch = 0x00010000;
        return;
    }
    // `s1`, the angle to the sound. It is ZERO unless the far arm below computes it, and the
    // doppler subtracts it either way - which is why it is a variable here and not a second call.
    int32_t ang = 0; // `move s1,zero` at 0x80019E50

    if (!(0x0000FFFF < d)) {
        // Under 1.0 world unit. NOTE: this arm jumps straight to the doppler test at 0x80019FC0 -
        // no atan2, no pan: the branch at 0x80019F3C puts the pan computation inside the if.
        *outVol = 127;
    } else {
        const int32_t d15 = Sra(d, 15); // 0x80019F40
        // The engine's standard clamp idiom, written out because it is four instructions and one
        // of them reconstructs its own input: max(127 - d15, 0) + min(d15, 0).
        const int32_t t = 127 - d15;                                  // 0x80019F54
        const int32_t hi = static_cast<int32_t>(static_cast<uint32_t>(t) & ~static_cast<uint32_t>(t >> 31));
        const int32_t u = 127 - t;                                    // 0x80019F64, = d15
        const int32_t lo = static_cast<int32_t>(static_cast<uint32_t>(u) & static_cast<uint32_t>(u >> 31));
        *outVol = static_cast<int32_t>(static_cast<uint32_t>(hi) + static_cast<uint32_t>(lo));

        ang = RatAtan2(dx, static_cast<int32_t>(0u - static_cast<uint32_t>(dz)),
                       env.atanTable); // 0x80019F7C
        const uint32_t players =
            static_cast<uint32_t>(env.gameState[kGameStatePlayers]) |
            (static_cast<uint32_t>(env.gameState[kGameStatePlayers + 1]) << 8) |
            (static_cast<uint32_t>(env.gameState[kGameStatePlayers + 2]) << 16) |
            (static_cast<uint32_t>(env.gameState[kGameStatePlayers + 3]) << 24);
        if (players == 1) { // 0x80019F98 - PAN IS COMPUTED IN A ONE-PLAYER GAME ONLY
            int32_t a = static_cast<int32_t>(static_cast<uint32_t>(li.S32(L + kListenerYaw)) -
                                             static_cast<uint32_t>(ang));
            a &= 0xFFF;
            a = (a >> 4) + 64;
            *outPan = a & 0xFF;
        }
    }

    if (outPitch == nullptr) return; // 0x80019FC0 - PlaySound3D always stops here

    // ---- the doppler, which nothing on the PlaySound3D path ever asks for
    const int32_t ex = static_cast<int32_t>(static_cast<uint32_t>(vx2) -
                                            static_cast<uint32_t>(li.S32(L + kListenerVx)));
    const int32_t ez = static_cast<int32_t>(static_cast<uint32_t>(vz2) -
                                            static_cast<uint32_t>(li.S32(L + kListenerVz)));
    const int32_t speed = OctagonalNorm(ex, ez);
    // The angle the RELATIVE motion makes, minus the angle to the sound. `s1` still holds the
    // latter, and on the `d <= 0xFFFF` arm it is 0 because RatAtan2 was never called.
    const int32_t ang2 =
        RatAtan2(ex, static_cast<int32_t>(0u - static_cast<uint32_t>(ez)), env.atanTable);
    const int32_t idx = static_cast<int32_t>((static_cast<uint32_t>(ang2) -
                                              static_cast<uint32_t>(ang)) & 0xFFFu);
    // `(i << 2) | 2` then `lh`: the COSINE member of the {s16 sin; s16 cos} pair.
    const int32_t c = env.sinCos[2 * idx + 1];
    const int32_t r = MultLo(Sra(speed, 8), Sra(c, 4)); // 0x8001A068
    const int32_t sum = static_cast<int32_t>(static_cast<uint32_t>(r) + 0x02000000u);
    if (sum > 0) { // blez a1 -> the negative arm
        *outPitch = static_cast<int32_t>(FixDiv(0x02000000u, static_cast<uint32_t>(sum)));
    } else {
        const uint32_t q = FixDiv(0x02000000u, 0u - static_cast<uint32_t>(sum));
        *outPitch = static_cast<int32_t>(0u - q);
    }
}

// ---------------------------------------------------------------------------- SLUS 0x8001F174
uint32_t StartVoice(int32_t bank, int32_t soundIndex, int32_t restart, int32_t reserved,
                    const SoundParams& params, const SoundSystemEnv& env) {
    const SoundBytes& S = env.state;
    const int32_t bankCount = S.S32(kSysBankCount);
    if (bankCount < bank) return 0; // 0x8001F1B8 - `slt`, so `bank == count` is ACCEPTED
    if (bank < 0) return 0;         // 0x8001F1C4
    if (static_cast<uint32_t>(bank) >= env.bankSlots) {
        MarkOutOfWindow(env.outOfWindow);
        return 0;
    }
    const SoundBankRef& b = env.banks[bank];
    if (b.address == 0) return 0; // 0x8001F1E0 - a null bank slot

    bool ok = true;
    const uint32_t soundOff = LookupSound(b, soundIndex, ok); // 0x8001F1E8
    if (!ok) {
        MarkOutOfWindow(env.outOfWindow);
        return 0;
    }
    if (soundOff == 0) return 0; // 0x8001F1F4

    // `restart` doubles as the descriptor index, but the only path that sees a non-zero value
    // first sets it to 0 (0x8001F1FC..0x8001F20C), so the descriptor is always descriptor 0.
    int32_t variation = restart;
    bool doRestart = false;
    if (variation != 0) {
        doRestart = true;
        variation = 0;
    }
    const uint32_t descOff = soundOff + static_cast<uint32_t>(12 * variation + 4);
    const uint32_t descAddr = b.address + descOff; // 0x8001F21C
    if ((descAddr & 3u) != 0) return 0;            // 0x8001F220 - an unaligned descriptor
    if (!b.Holds(descOff, 8)) {                    // +0x00 volume and +0x06 pitch
        MarkOutOfWindow(env.outOfWindow);
        return 0;
    }
    const int32_t descVol = static_cast<int32_t>(b.data[descOff]); // lbu v1,0(s2)

    int32_t left = 0;
    int32_t right = 0;
    if (S.S32(kSysPanDisabled) != 0) {  // 0x8001F234 - mono
        const int32_t V = MultLo(params.volume, descVol);
        left = V;                       // both `mflo`s read the same LO
        right = V;
    } else {
        const int32_t pan = params.pan;             // lw a0,8(s4); t0 keeps the original
        const bool front = pan < 129;               // slti a3,a0,129 - SIGNED
        const int32_t f = front ? pan : (256 - pan);
        const int32_t V = MultLo(params.volume, descVol);
        const int32_t praw = MultLo(f - 64, V);     // 0x8001F274
        const int32_t P = Sra(praw, 6);             // 0x8001F280
        // max(P, 0), written as the original writes it: `negu; sra 31; and`.
        const int32_t posMask = static_cast<int32_t>(0u - static_cast<uint32_t>(P)) >> 31;
        const int32_t pPos = static_cast<int32_t>(static_cast<uint32_t>(P) &
                                                  static_cast<uint32_t>(posMask));
        int32_t l = static_cast<int32_t>(static_cast<uint32_t>(V) - static_cast<uint32_t>(pPos));
        // 0x8001F298: an unconditional `xor` with the sign of the ORIGINAL pan word, then a
        // `negu` only on the back half. A negative pan is not something the game ever passes, but
        // it is what these two instructions say, so it is what this does.
        l = static_cast<int32_t>(static_cast<uint32_t>(l) ^ static_cast<uint32_t>(pan >> 31));
        if (!front) l = static_cast<int32_t>(0u - static_cast<uint32_t>(l)); // 0x8001F29C
        left = l;
        const int32_t negMask = praw >> 31;         // 0x8001F2A4 - the sign of the UNSHIFTED term
        const int32_t pNeg = static_cast<int32_t>(static_cast<uint32_t>(P) &
                                                  static_cast<uint32_t>(negMask));
        right = static_cast<int32_t>(static_cast<uint32_t>(V) + static_cast<uint32_t>(pNeg));
    }

    const int32_t v = AllocVoice(reserved, env); // 0x8001F2CC
    if (v < 0) return 0;                         // 0x8001F2D8 - the allocator refused
    const uint32_t voiceOff = kVoiceStride * static_cast<uint32_t>(v);
    const SoundBytes& vv = env.voices;

    vv.SetU32(voiceOff + kVoiceDesc, descAddr);                  // 0x8001F2E0
    if (params.pitch == -1)                                      // 0x8001F2EC
        vv.SetU32(voiceOff + kVoicePitch, b.U16(descOff + 6));   // lhu v0,6(s2) - zero-extended
    else
        vv.SetU32(voiceOff + kVoicePitch, static_cast<uint32_t>(params.pitch));
    vv.SetU32(voiceOff + kVoiceVolL, static_cast<uint32_t>(left));   // 0x8001F304
    vv.SetU32(voiceOff + kVoiceVolR, static_cast<uint32_t>(right));  // 0x8001F30C

    const int32_t channel = vv.S32(voiceOff + kVoiceChannel);
    if (doRestart && env.restart != nullptr) env.restart->RestartVoice(channel); // 0x8001F31C

    const uint32_t serial = vv.U32(voiceOff + kVoiceSerial);
    const uint32_t handle = Sllv(static_cast<uint32_t>(channel), 27) + (serial & 0x07FFFFFFu);
    SoundKeyOn(S, Sllv(1u, channel)); // 0x8001F344
    return handle;
}

// ---------------------------------------------------------------------------- SLUS 0x80017BA0
void PlaySound3D(int32_t x, int32_t z, int32_t soundIndex, int32_t bank,
                 const PlaySound3DEnv& env) {
    if (bank == env.mutedBank) return; // 0x80017BD0 - the muted-bank sentinel
    const uint8_t* gs = env.listener.gameState;
    if (gs == nullptr) return;
    auto players = [gs]() {
        return static_cast<uint32_t>(gs[kGameStatePlayers]) |
               (static_cast<uint32_t>(gs[kGameStatePlayers + 1]) << 8) |
               (static_cast<uint32_t>(gs[kGameStatePlayers + 2]) << 16) |
               (static_cast<uint32_t>(gs[kGameStatePlayers + 3]) << 24);
    };
    if (players() == 0) return; // 0x80017BEC

    uint32_t p = 0;      // s0
    int32_t bnk = bank;  // s1 - KEPT across iterations, so the default substitution happens once
    for (;;) {           // 0x80017BFC .. 0x80017CC4
        int32_t vol = 0;
        int32_t pan = 0;
        if (x != 0 || z != 0) { // 0x80017BFC / 0x80017C04
            Sound3DParams(static_cast<int32_t>(p), x, z, 0, 0, &vol, &pan, nullptr, 0,
                          env.listener);
        } else {
            vol = 127; // 0x80017C48
            pan = 64;  // 0x80017C50
            p = players(); // 0x80017C54 - ... and stop after this one
        }
        SoundParams sp;
        sp.pitch = -1;                                 // 0x80017C70
        sp.volume = Sra(MultLo(vol, env.master3d), 7); // 0x80017C64..0x80017C7C
        sp.pan = pan;                                  // 0x80017C74
        if (bnk == 0) bnk = env.defaultBank;           // 0x80017C80..0x80017C88
        StartVoice(bnk, soundIndex, 0, 0, sp, env.system); // 0x80017CA4
        p = p + 1;                                     // 0x80017CBC
        if (!(p < players())) break;                   // 0x80017CC0 - `sltu`, and a RE-READ
    }
}

// ---------------------------------------------------------------------------- SLUS 0x80016768
void SetListener(int32_t p, int32_t x, int32_t z, int32_t vx, int32_t vz, int32_t camYaw,
                 const SoundBytes& listeners) {
    const uint32_t L = kListenerStride * static_cast<uint32_t>(p);
    if (!listeners.valid() || static_cast<uint64_t>(kListenerStride) * static_cast<uint32_t>(p) + kListenerStride > listeners.size()) return;
    listeners.SetU32(L + kListenerYaw,
                     static_cast<uint32_t>(2048 - camYaw)); // 0x80016784/88, half a turn of 4096
    listeners.SetU32(L + kListenerX, static_cast<uint32_t>(x));
    listeners.SetU32(L + kListenerZ, static_cast<uint32_t>(z));
    listeners.SetU32(L + kListenerVx, static_cast<uint32_t>(vx));
    listeners.SetU32(L + kListenerVz, static_cast<uint32_t>(vz));
}

// ---------------------------------------------------------------------------- SLUS 0x80017B30
uint32_t SurfaceSound(int32_t k, const uint8_t* table52) {
    // `a = k & ~(k>>31)` and `b = (51-k) & ((51-k)>>31)`, i.e. max(k,0) + min(51-k,0).
    const int32_t a = static_cast<int32_t>(static_cast<uint32_t>(k) &
                                           ~static_cast<uint32_t>(k >> 31));
    const int32_t t = 51 - k;
    const int32_t b = static_cast<int32_t>(static_cast<uint32_t>(t) & static_cast<uint32_t>(t >> 31));
    const int32_t i = static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
    return table52[i]; // lbu - zero-extended into v0
}

// ---------------------------------------------------------------------------- SLUS 0x80017B6C
void QueueListenerSound(int32_t a, int32_t id, int32_t t, int32_t p,
                        const SoundBytes& listeners) {
    const uint32_t L = kListenerStride * static_cast<uint32_t>(p);
    if (!listeners.valid() || static_cast<uint64_t>(kListenerStride) * static_cast<uint32_t>(p) + kListenerStride > listeners.size()) return;
    const uint32_t s = (id == 109) ? 4u : 0u; // 0x80017B6C/70 - slot 1 is reserved for id 109
    listeners.SetU32(L + kListenerQueueId + s, static_cast<uint32_t>(id));
    listeners.SetU32(L + kListenerQueueArg + s, static_cast<uint32_t>(a));
    listeners.SetU32(L + kListenerQueueTtl + s, static_cast<uint32_t>(t));
}

} // namespace rr::sim
