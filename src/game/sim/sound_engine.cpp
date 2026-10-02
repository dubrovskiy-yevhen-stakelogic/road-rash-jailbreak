// The engine note and everything under it - see sound_engine.h. Transcribed from the replay-proven
// Python model of tools\scout\engine_note.py (m_note, m_start, m_road) and from our own disassembly
// of the helpers and of the libspu pieces; every load, store and call below is in the ORIGINAL's
// order, including the re-reads of the engine record after each callee, because that order is what
// the bench's whole-RAM comparison sees when two structures alias.
#include "game/sim/sound_engine.h"

#include <vector>

#include "game/sim/fixed.h"

namespace rr::sim {
namespace {

constexpr uint32_t kMask27 = 0x07FFFFFFu;
constexpr uint32_t kVoiceStride = 44;
constexpr uint32_t kRamBytes = 2u * 1024u * 1024u;
constexpr uint32_t kBankWindowBytes = 4096; // the same window the stage-A rows hand the emitter
constexpr uint32_t kListenerRecordsViewed = 4;

int32_t S(uint32_t v) { return static_cast<int32_t>(v); }
uint32_t U(int32_t v) { return static_cast<uint32_t>(v); }
int32_t MultLo(int32_t a, int32_t b) { return S(U(a) * U(b)); }
int32_t Sra(int32_t v, int32_t n) { return v >> (n & 31); }
uint32_t Sllv(uint32_t v, uint32_t n) { return v << (n & 31u); }
// `sra 31 / addu / xor` - so abs(INT32_MIN) stays INT32_MIN.
int32_t AbsW(int32_t x) {
    const int32_t m = x >> 31;
    return S((U(x) + U(m)) ^ U(m));
}
// The compiler's branchless clamp to 0..127: max(x, 0) + min(127 - x, 0).
int32_t Clamp127(int32_t x) {
    const int32_t a = S(U(x) & ~U(x >> 31));
    const int32_t t = S(127u - U(x));
    return S(U(a) + (U(t >> 31) & U(t)));
}

// Main RAM as the interpreter maps it: KUSEG/KSEG0/KSEG1, first 8 MiB, mirrored every 2 MiB.
bool IsRam(uint32_t a) {
    if (a >= 0xC0000000u) return false;
    return (a & 0x1FFFFFFFu) < 0x00800000u;
}
uint32_t RamOffset(uint32_t a) { return a & (kRamBytes - 1u); }

// A 16-bit access that is RAM on one input and a hardware register on another - libspu's base
// pointer and the shadow flag decide which. RAM goes through `m` (which faults exactly where the
// console would), anything else through `io`.
uint16_t Load16(SoundMachine& s, uint32_t a) {
    if (IsRam(a)) return s.m.U16(a);
    if (a & 1u) {
        s.Fail("misaligned halfword load from a hardware register", a);
        return 0;
    }
    uint16_t v = 0;
    if (!s.io.Load16(a, v)) {
        s.Fail("a hardware register load this answerer does not serve", a);
        return 0;
    }
    return v;
}
void Store16(SoundMachine& s, uint32_t a, uint32_t v) {
    if (IsRam(a)) {
        s.m.W16(a, static_cast<uint16_t>(v));
        return;
    }
    if (a & 1u) {
        s.Fail("misaligned halfword store to a hardware register", a);
        return;
    }
    if (!s.io.Store16(a, static_cast<uint16_t>(v))) s.Fail("a hardware register store this answerer does not serve", a);
}

uint32_t VoiceOf(SoundMachine& s, uint32_t handle) {
    const uint32_t voices = s.m.U32(kSoundSystem + 0x0C);
    return voices + kVoiceStride * (handle >> 27);
}

void KeyOff(SoundMachine& s, uint32_t mask) { // SLUS 0x8001EB44: S[+0x18] |= mask
    s.m.W32(kSoundSystem + 0x18, s.m.U32(kSoundSystem + 0x18) | mask);
}

// A byte view straight onto guest memory, or an invalid one (and a failure) when the block would
// leave RAM.
SoundBytes View(SoundMachine& s, uint32_t a, uint32_t n) {
    if (!IsRam(a)) {
        s.Fail("a sound structure outside RAM", a);
        return SoundBytes();
    }
    const uint32_t off = RamOffset(a);
    if (off + n > kRamBytes) {
        s.Fail("a sound structure running past the end of RAM", a);
        return SoundBytes();
    }
    return SoundBytes(s.ram + off, n);
}

// SLUS 0x80050678(0, 1 << channel), the `noReverb` arm of StartVoice (0x8001F31C).
class ReverbOffSink final : public VoiceRestartSink {
public:
    explicit ReverbOffSink(SoundMachine& s) : s_(s) {}
    void RestartVoice(int32_t channel) override {
        SpuSetReverbVoice(s_, 0, Sllv(1u, U(channel)));
    }

private:
    SoundMachine& s_;
};

// The stage-A emitter's environment, built over guest memory: views onto the record, the voice
// table and the banks, with no copies, so the ported emitter and this file see one state.
struct SystemView {
    std::vector<SoundBankRef> banks;
    uint32_t serial = 0;
    bool outOfWindow = false;
};
SoundSystemEnv BuildSystem(SoundMachine& s, SystemView& v, VoiceRestartSink* sink) {
    SoundSystemEnv e;
    e.state = View(s, kSoundSystem, 0x188);
    const uint32_t voices = s.m.U32(kSoundSystem + 0x0C);
    e.voices = View(s, voices, kVoiceStride * 24u);
    v.serial = s.m.U32(s.m.gp() + kSerialGp);
    e.serial = &v.serial;
    const int32_t count = s.m.S32(kSoundSystem + 0x00);
    const uint32_t slots = (count >= 0 && count < 64) ? U(count) + 1u : 0u;
    const uint32_t table = s.m.U32(kSoundSystem + 0x04);
    v.banks.assign(slots, SoundBankRef{});
    for (uint32_t i = 0; i < slots; ++i) {
        const uint32_t p = s.m.U32(table + 4u * i);
        v.banks[i].address = p;
        if (!IsRam(p)) continue; // "not null" to the original; any read of it is out of window
        const uint32_t off = RamOffset(p);
        v.banks[i].data = s.ram + off;
        v.banks[i].size = (kRamBytes - off < kBankWindowBytes) ? kRamBytes - off : kBankWindowBytes;
    }
    e.banks = v.banks.empty() ? nullptr : v.banks.data();
    e.bankSlots = slots;
    e.restart = sink;
    e.outOfWindow = &v.outOfWindow;
    return e;
}

struct ListenerView {
    int32_t atan[20]{};
    std::vector<int16_t> sinCos;
    bool outOfWindow = false;
};
Sound3DEnv BuildListener(SoundMachine& s, ListenerView& v, bool doppler) {
    Sound3DEnv e;
    const uint32_t L = s.m.U32(s.m.gp() + kListenerGp);
    if (L != 0) {
        uint32_t n = kListenerRecordsViewed * 72u;
        if (IsRam(L) && RamOffset(L) + n > kRamBytes) n = kRamBytes - RamOffset(L);
        e.listeners = View(s, L, n);
    }
    const uint32_t gs = s.m.U32(kSoundGameStatePtr);
    if (IsRam(gs) && RamOffset(gs) + 0x40 <= kRamBytes) e.gameState = s.ram + RamOffset(gs);
    for (uint32_t i = 0; i < 20; ++i) v.atan[i] = s.m.S32(kAtanTableAddr + 4u * i);
    e.atanTable = v.atan;
    if (doppler) {
        v.sinCos.resize(8192);
        for (uint32_t i = 0; i < 8192; ++i) v.sinCos[i] = s.m.S16(kSinCosAddr + 2u * i);
        e.sinCos = v.sinCos.data();
    }
    e.outOfWindow = &v.outOfWindow;
    return e;
}

} // namespace

// ============================================================================ libspu
uint32_t SpuSetAnyVoice(SoundMachine& s, int32_t on, uint32_t mask, uint32_t r, uint32_t r1) {
    GuestRam& m = s.m;
    // 0x800506A8..0x800506E8: pick the shadow or the SPU, then read r1 FIRST and r second.
    const bool shadow0 = (m.U32(kSpuShadowFlag) & 1u) != 0;
    const uint32_t base0 = shadow0 ? kSpuShadow : m.U32(kSpuBasePtr);
    const uint32_t hi0 = Load16(s, base0 + 2u * r1);
    const uint32_t lo0 = Load16(s, base0 + 2u * r);
    uint32_t t2 = lo0 | ((hi0 & 0xFFu) << 16);

    auto dirty = [&]() { // 0x80050774..0x80050794: *(0x8005A3D4) |= 1 << ((r - 198) >> 1)
        const int32_t bit = S(r - 198u) >> 1;
        m.W32(kSpuDirtyMask, m.U32(kSpuDirtyMask) | Sllv(1u, U(bit)));
    };
    if (on == 1) { // 0x8005072C
        const bool shadow = (m.U32(kSpuShadowFlag) & 1u) != 0;
        const uint32_t base = shadow ? kSpuShadow : m.U32(kSpuBasePtr);
        const uint32_t lo = Load16(s, base + 2u * r) | mask;
        Store16(s, base + 2u * r, lo);
        const uint32_t hi = Load16(s, base + 2u * r1) | ((mask >> 16) & 0xFFu);
        Store16(s, base + 2u * r1, hi);
        if (shadow) dirty();
        t2 |= mask & 0x00FFFFFFu;
    } else if (on == 0) { // 0x800507E8
        const bool shadow = (m.U32(kSpuShadowFlag) & 1u) != 0;
        const uint32_t base = shadow ? kSpuShadow : m.U32(kSpuBasePtr);
        const uint32_t lo = Load16(s, base + 2u * r) & ~mask;
        Store16(s, base + 2u * r, lo);
        const uint32_t hi = Load16(s, base + 2u * r1) & ~((mask >> 16) & 0xFFu);
        Store16(s, base + 2u * r1, hi);
        if (shadow) dirty();
        t2 &= ~(mask & 0x00FFFFFFu);
    } else if (on == 8) { // 0x800508BC
        const bool shadow = (m.U32(kSpuShadowFlag) & 1u) != 0;
        const uint32_t base = shadow ? kSpuShadow : m.U32(kSpuBasePtr);
        Store16(s, base + 2u * r, mask);
        Store16(s, base + 2u * r1, (mask >> 16) & 0xFFu);
        if (shadow) dirty();
        t2 = mask & 0x00FFFFFFu;
    }
    return t2 & 0x00FFFFFFu; // 0x8005095C
}

uint32_t SpuWriteAddress(SoundMachine& s, int32_t reg, uint32_t addr) {
    GuestRam& m = s.m;
    if (m.U32(kSpuAlignOn) != 0) { // 0x8004F0B4
        const uint32_t unit = m.U32(kSpuAlignUnit);
        if (unit == 0) { // `break 7` at 0x8004F0D4 - the console's divide-by-zero trap
            s.Fail("SpuWriteAddress divides by a zero alignment unit (break 7)", 0x8004F0D4u);
            return 0;
        }
        if (addr % unit != 0) addr = (addr + unit) & ~m.U32(kSpuAlignMask);
    }
    const uint32_t shifted = addr >> (m.U32(kSpuAlignShift) & 31u); // srlv
    if (reg == -2) return addr;
    if (reg == -1) return shifted & 0xFFFFu;
    Store16(s, m.U32(kSpuBasePtr) + 2u * U(reg), shifted);
    return addr;
}

uint32_t SpuSetVoiceAttr(SoundMachine& s, uint32_t channel, const SpuVoiceAttr& a) {
    GuestRam& m = s.m;
    const uint32_t mask = a.mask;
    constexpr uint32_t kPorted = 0x1u | 0x2u | 0x10u | 0x80u | 0x10000u | 0x20000u | 0x40000u;
    if (mask == 0 || (mask & ~kPorted) != 0) {
        s.Fail("SpuSetVoiceAttr attribute bits that are not ported", mask);
        return 0;
    }
    const uint32_t v8 = channel << 3; // s3
    if (mask & 0x10u) Store16(s, (channel << 4) + m.U32(kSpuBasePtr) + 4u, a.pitch);         // 0x80051C8C
    if (mask & 0x1u) Store16(s, (v8 << 1) + m.U32(kSpuBasePtr) + 0u, a.volL & 0x7FFFu);      // 0x80051DDC
    if (mask & 0x2u) Store16(s, (v8 << 1) + m.U32(kSpuBasePtr) + 2u, a.volR & 0x7FFFu);      // 0x80051EBC
    if (mask & 0x80u) SpuWriteAddress(s, S(v8 | 3u), a.addr);                                  // 0x80051ED4
    if (mask & 0x10000u) SpuWriteAddress(s, S(v8 | 7u), a.loopAddr);                           // 0x80051EF4
    if (mask & 0x20000u) Store16(s, (v8 << 1) + m.U32(kSpuBasePtr) + 8u, a.adsr1);           // 0x80051F24
    if (mask & 0x40000u) Store16(s, (v8 << 1) + m.U32(kSpuBasePtr) + 10u, a.adsr2);          // 0x80051F50
    return 0; // the delay loop at 0x800521A0 leaves `slti` = 0 in v0
}

uint32_t SpuSetKey(SoundMachine& s, int32_t on, uint32_t mask) {
    GuestRam& m = s.m;
    const uint32_t a1 = mask & 0x00FFFFFFu;
    const uint32_t a2 = a1 >> 16;
    if (on == 1) {                                                       // 0x80050D28
        if (m.U32(kSpuShadowFlag) & 1u) {
            const uint32_t a0 = kSpuKeyShadow;
            m.W16(a0 + 0, static_cast<uint16_t>(a1));
            m.W16(a0 + 2, static_cast<uint16_t>(a2));
            m.W32(kSpuDirtyMask, m.U32(kSpuDirtyMask) | 1u);
            m.W32(kSpuKeyDirty, m.U32(kSpuKeyDirty) | a1);
            if (m.U16(a0 + 4) & a1) m.W16(a0 + 4, static_cast<uint16_t>(m.U16(a0 + 4) & ~a1));
            const uint32_t k = m.U16(a0 + 6) & a2;
            if (k == 0) return 0;
            const uint32_t v = m.U16(a0 + 6) & ~a2;
            m.W16(a0 + 6, static_cast<uint16_t>(v));
            return v;
        }
        const uint32_t keyed = m.U32(kSpuKeyedMask) | a1;                // 0x80050DCC
        const uint32_t base = m.U32(kSpuBasePtr);
        Store16(s, base + 392u, a1);                                     // KON lo, 0x1F801D88
        Store16(s, base + 394u, a2);                                     // KON hi
        m.W32(kSpuKeyedMask, keyed);
        return keyed;
    }
    if (on != 0) return 1;                                               // 0x80050D20
    if (m.U32(kSpuShadowFlag) & 1u) {                                    // 0x80050E04
        const uint32_t a0 = kSpuKeyShadow;
        m.W16(a0 + 4, static_cast<uint16_t>(a1));
        m.W16(a0 + 6, static_cast<uint16_t>(a2));
        m.W32(kSpuDirtyMask, m.U32(kSpuDirtyMask) | 1u);
        m.W32(kSpuKeyDirty, m.U32(kSpuKeyDirty) & ~a1);
        if (m.U16(a0 + 0) & a1) m.W16(a0 + 0, static_cast<uint16_t>(m.U16(a0 + 0) & ~a1));
        const uint32_t k = m.U16(a0 + 2) & a2;
        if (k == 0) return 0;
        const uint32_t v = m.U16(a0 + 2) & ~a2;
        m.W16(a0 + 2, static_cast<uint16_t>(v));
        return v;
    }
    const uint32_t base = m.U32(kSpuBasePtr);                            // 0x80050E90
    Store16(s, base + 396u, a1);                                         // KOFF lo, 0x1F801D8C
    Store16(s, base + 398u, a2);                                         // KOFF hi
    const uint32_t keyed = m.U32(kSpuKeyedMask) & ~a1;
    m.W32(kSpuKeyedMask, keyed);
    return keyed;
}

// ============================================================================ the helpers
uint32_t GetRCnt(SoundMachine& s, uint32_t id) {
    const uint32_t k = id & 0xFFFFu;
    if (!(k < 3)) return 0;
    return Load16(s, (k << 4) + s.m.U32(kRootCounterBase));
}

uint32_t GuestLookupSound(SoundMachine& s, uint32_t bank, int32_t index) {
    const int32_t count = s.m.U8(bank + 4);
    if (!(index < count)) return 0;
    if (index < 0) return 0;
    const uint32_t o = s.m.U32((U(index) << 2) + bank + 16u);
    return o ? bank + o : 0;
}

uint32_t GetSoundPitch(SoundMachine& s, int32_t bank, int32_t sound, int32_t desc) {
    if (s.m.S32(kSoundSystem + 0x00) < bank) return 0; // `slt` - bank == count is accepted
    if (bank < 0) return 0;
    const uint32_t b = s.m.U32((U(bank) << 2) + s.m.U32(kSoundSystem + 0x04));
    if (b == 0) return 0;
    const uint32_t snd = GuestLookupSound(s, b, sound);
    if (snd == 0) return 0;
    return s.m.U16(U(desc) * 12u + snd + 10u);
}

uint32_t ReleaseVoice(SoundMachine& s, uint32_t voice) {
    GuestRam& m = s.m;
    const uint32_t ch = m.U32(voice + 28);
    m.W32(voice + 4, 0);
    m.W32(voice + 16, 0);
    for (uint32_t i = 0; i < 22; ++i) {
        const uint32_t slot = kSoundSystem + 88u + 4u * i;
        if (m.U32(slot) != ch) continue;
        m.W32(slot, 0xFFFFFFFFu);
        const int32_t head = m.S32(kSoundSystem + 376);
        m.W32(U(head) * 4u + kSoundSystem + 272u, ch);
        const int32_t next = head + 1;
        const uint32_t lt = next < 25 ? 1u : 0u;
        m.W32(kSoundSystem + 376, U(next));
        m.W32(kSoundSystem + 376, lt ? U(next) : 0u);
        return lt;
    }
    return 0;
}

uint32_t StopVoice(SoundMachine& s, uint32_t handle) {
    const uint32_t v = VoiceOf(s, handle);
    const uint32_t serial = s.m.U32(v + 4) & kMask27;
    if (serial != (handle & kMask27)) return serial;
    KeyOff(s, Sllv(1u, s.m.U32(v + 28)));
    ReleaseVoice(s, v);
    return SpuSetReverbVoice(s, 1, Sllv(1u, s.m.U32(v + 28)));
}

uint32_t SetVoiceLoopOffset(SoundMachine& s, int32_t offset, uint32_t handle) {
    const uint32_t v = VoiceOf(s, handle);
    if ((s.m.U32(v + 4) & kMask27) != (handle & kMask27)) return 1;
    const uint32_t desc = s.m.U32(v + 8);
    const uint32_t ch = s.m.U32(v + 28);
    SpuVoiceAttr a;
    a.mask = 0x10000u;
    a.loopAddr = s.m.U32(desc + 8) + U(offset);
    (void)ch; // the attribute's voice mask `1 << ch` is written but SpuSetVoiceAttr never reads it
    return SpuSetVoiceAttr(s, s.m.U32(v + 28), a);
}

uint32_t SetVoicePitchMod(SoundMachine& s, uint32_t handle, int32_t on) {
    return SpuSetPitchLfoVoice(s, on != 0 ? 1 : 0, Sllv(1u, handle >> 27));
}

uint32_t UpdateVoice(SoundMachine& s, uint32_t handle, const SoundParams& p) {
    GuestRam& m = s.m;
    const uint32_t v = VoiceOf(s, handle);
    const uint32_t serial = m.U32(v + 4) & kMask27;
    if (serial != (handle & kMask27)) return serial; // STALE -> no-op
    int32_t left = 0, right = 0;
    if (m.U32(kSoundSystem + 0x184) == 0) {
        int32_t pan = p.pan;
        const bool front = pan < 129;
        const int32_t t1 = pan;
        if (!front) pan = 256 - pan;
        const int32_t V = MultLo(p.volume, m.U8(m.U32(v + 8)));
        const int32_t a3 = MultLo(pan - 64, V);
        const int32_t P = a3 >> 6;
        const int32_t pos = S(U(P) & U(S(0u - U(P)) >> 31));
        int32_t l = S((U(V) - U(pos)) ^ U(t1 >> 31));
        if (!front) l = S(0u - U(l));
        left = l;
        right = S(U(V) + (U(P) & U(a3 >> 31)));
    } else {
        const int32_t V = MultLo(p.volume, m.U8(m.U32(v + 8)));
        left = V;
        right = V;
    }
    SpuVoiceAttr a;
    a.mask = 0x13u;
    a.volL = static_cast<uint16_t>(left);
    a.volR = static_cast<uint16_t>(right);
    a.pitch = static_cast<uint16_t>(U(p.pitch));
    return SpuSetVoiceAttr(s, m.U32(v + 28), a);
}

uint32_t ProgramVoice(SoundMachine& s, uint32_t voice, uint32_t pitch, uint32_t volL, uint32_t volR) {
    GuestRam& m = s.m;
    const uint32_t desc = m.U32(voice + 8);
    SpuVoiceAttr a;
    a.mask = 0x00060093u;
    a.volL = static_cast<uint16_t>(volL);
    a.volR = static_cast<uint16_t>(volR);
    a.pitch = static_cast<uint16_t>(pitch);
    a.addr = m.U32(desc + 8);
    a.adsr1 = m.U16(desc + 2);
    a.adsr2 = m.U16(desc + 4);
    return SpuSetVoiceAttr(s, m.U32(voice + 28), a);
}

uint32_t SoundService(SoundMachine& s) {
    GuestRam& m = s.m;
    const uint32_t S0 = kSoundSystem;
    if (m.U32(S0 + 0x10) != 0) return 1;                                 // the re-entrancy guard
    const uint32_t off = m.U32(S0 + 0x18);
    m.W32(S0 + 0x10, 1);
    if (off != 0) {
        SpuSetKey(s, 0, off);                                            // 0x8001EEDC
        m.W32(S0 + 0x18, 0);
    }
    if (m.U32(S0 + 0x14) != 0) {
        int32_t n = m.S32(S0 + 0x08) - 1;
        uint32_t voice = m.U32(S0 + 0x0C);
        while (n != -1) {
            const uint32_t ch = m.U32(voice + 28);
            if (Sllv(1u, ch) & m.U32(S0 + 0x14))
                ProgramVoice(s, voice, m.U32(voice + 32), m.U32(voice + 36), m.U32(voice + 40)); // 0x8001EF3C
            m.W32(voice + 16, 2);                                        // for EVERY voice
            voice += kVoiceStride;
            --n;
            if (s.Faulted()) break; // a voice count the console would loop on for hours
        }
        const uint32_t on = m.U32(S0 + 0x14);
        if (on != 0) SpuSetKey(s, 1, on);                                // 0x8001EF70
    }
    m.W32(S0 + 0x14, 0);
    m.W32(S0 + 0x10, 0);
    return S0;
}

uint32_t AudioVSyncTick(SoundMachine& s) {
    GuestRam& m = s.m;
    const uint32_t gp = m.gp();
    if (m.U32(kAudioGate) & 4u) {
        const uint32_t gs = m.U32(kSoundGameStatePtr);
        bool service = true;
        if (m.S32(gs + 0x30) != 2) {
            const uint32_t t = m.U32(gp + kTickToggleGp);
            if (t != 0) {                                                // 1P: only player 0
                m.W32(gp + kTickToggleGp, t ^ 1u);
                service = false;
            }
        }
        if (service) {
            const uint32_t E = m.U32(gp + kEngineRecGp) + kEngineRecBytes * m.U32(gp + kTickToggleGp);
            const int32_t rate = m.S32(E + 0x38);
            const int32_t lvl = m.S32(E + 0x30);
            const int32_t tgt = m.S32(E + 0x34);
            const int32_t l1 = S(U(lvl) + U(rate));
            const int32_t prod = MultLo(S(U(l1) - U(tgt)), rate);
            m.W32(E + 0x30, U(l1));
            const int32_t a2 = (prod < 0) ? l1 : tgt;
            const int32_t tgt2 = m.S32(E + 0x64);
            const int32_t rate2 = m.S32(E + 0x68);
            const int32_t prod2 = MultLo(S(U(l1) - U(tgt2)), rate2);   // the FIRST ramp's level
            const int32_t lv2 = m.S32(E + 0x60);
            m.W32(E + 0x30, U(a2));
            const int32_t s2 = S(U(lv2) + U(rate2));
            m.W32(E + 0x60, U(s2));
            const int32_t v1 = (prod2 < 0) ? s2 : tgt2;
            const uint32_t en = m.U8(E + 4);
            m.W32(E + 0x60, U(v1));
            if (en) {
                const uint32_t bk = m.U32(E);
                SoundParams sp;
                sp.pan = m.S32(E + 0x74);
                const uint32_t fl = m.U32(bk + 0x234);
                const int32_t hurt = S((fl >> 9) & 1u);
                const int32_t lv = m.S32(E + 0x30);                      // the CLAMPED level
                int32_t pv = S(U(lv) + (U(-hurt) & U(lv >> 2)));
                pv = MultLo(pv, m.S32(E + 0x78)) >> 16;
                sp.pitch = pv;
                const int32_t f70 = m.S32(E + 0x70);
                sp.volume = hurt ? f70 : (MultLo(f70, s2) >> 7);         // s2 UNclamped
                UpdateVoice(s, m.U32(E + 0x10), sp);                     // LAYER 0
                if (m.U32(E + 0x14) != 0) {
                    if (hurt) {
                        const int32_t pp = sp.pitch;
                        sp.volume = f70;
                        sp.pitch = (pp >> 1) + (pp >> 2);
                    } else {
                        sp.volume = MultLo(f70, 127 - s2) >> 7;
                    }
                    UpdateVoice(s, m.U32(E + 0x14), sp);                 // LAYER 1
                } else {
                    const int32_t g = S(GetSoundPitch(s, m.S32(E + 8), 4, 0));
                    const int32_t f78 = m.S32(E + 0x78);
                    sp.volume = f70;
                    sp.pitch = MultLo(f78, g) >> 16;
                    UpdateVoice(s, m.U32(E + 0x18), sp);                 // LAYER 2
                }
                int32_t k = m.S32(E + 0x30) >> 4;
                if (!(k < 128)) k = 127;
                sp.volume = MultLo(f70, k) >> 7;
                {
                    const uint32_t P = m.U32(E + 0x2C);
                    const int32_t pp = MultLo(sp.pitch, m.S32(P + 0x0C)) >> 16;
                    sp.pitch = MultLo(pp, m.S32(E + 0x78)) >> 16;
                }
                UpdateVoice(s, m.U32(E + 0x1C), sp);                     // LAYER 3
                sp.volume = 0;
                {
                    const uint32_t P = m.U32(E + 0x2C);
                    const int32_t l = m.S32(E + 0x30);
                    const int32_t sh = m.S32(P + 0x10);
                    const int32_t v = S(U(Sra(l, sh)) + U(m.S32(E + 0x40)));
                    sp.pitch = MultLo(v, m.S32(E + 0x78)) >> 16;
                }
                UpdateVoice(s, m.U32(E + 0x20), sp);                     // LAYER 4: PMON source
            }
            m.W32(gp + kTickToggleGp, m.U32(gp + kTickToggleGp) ^ 1u);
        }
    }
    return SoundService(s);                                              // 0x80019C30
}

// ============================================================================ the emitter over guest memory
uint32_t GuestStartVoice(SoundMachine& s, int32_t bank, int32_t sound, int32_t noReverb,
                         int32_t reserved, const SoundParams& params) {
    SystemView v;
    ReverbOffSink sink(s);
    const SoundSystemEnv e = BuildSystem(s, v, &sink);
    if (s.failed) return 0;
    const uint32_t h = StartVoice(bank, sound, noReverb, reserved, params, e);
    s.m.W32(s.m.gp() + kSerialGp, v.serial);
    if (v.outOfWindow) s.Fail("StartVoice read outside the bank window", U(bank));
    return h;
}

uint32_t GuestAllocVoice(SoundMachine& s, int32_t reserved) {
    SystemView v;
    const SoundSystemEnv e = BuildSystem(s, v, nullptr);
    if (s.failed) return 0;
    const int32_t index = AllocVoice(reserved, e);
    s.m.W32(s.m.gp() + kSerialGp, v.serial);
    if (v.outOfWindow) s.Fail("AllocVoice read outside its window", U(reserved));
    if (index < 0) return 0;
    return s.m.U32(kSoundSystem + 0x0C) + kVoiceStride * U(index);
}

void GuestSound3DParams(SoundMachine& s, int32_t p, int32_t x, int32_t z, int32_t vx2, int32_t vz2,
                        int32_t* outVol, int32_t* outPan, int32_t* outPitch, int32_t shift) {
    ListenerView v;
    const Sound3DEnv e = BuildListener(s, v, outPitch != nullptr);
    if (s.failed) return;
    Sound3DParams(p, x, z, vx2, vz2, outVol, outPan, outPitch, shift, e);
    if (v.outOfWindow) s.Fail("Sound3DParams read outside the listener window", U(p));
}

void GuestPlaySound3D(SoundMachine& s, int32_t x, int32_t z, int32_t sound, int32_t bank) {
    ListenerView lv;
    SystemView sv;
    ReverbOffSink sink(s);
    PlaySound3DEnv e;
    e.mutedBank = s.m.S32(s.m.gp() + kRoadBankGp);
    e.defaultBank = s.m.S32(s.m.gp() + kDefaultBankGp);
    e.master3d = s.m.S32(kEffectsSlider);
    e.listener = BuildListener(s, lv, false);
    e.system = BuildSystem(s, sv, &sink);
    if (s.failed) return;
    PlaySound3D(x, z, sound, bank, e);
    s.m.W32(s.m.gp() + kSerialGp, sv.serial);
    if (lv.outOfWindow || sv.outOfWindow) s.Fail("PlaySound3D read outside its windows", U(sound));
}

// ============================================================================ the engine note
namespace {
uint32_t EngineRec(SoundMachine& s, int32_t p) {
    return s.m.U32(s.m.gp() + kEngineRecGp) + kEngineRecBytes * U(p);
}
} // namespace

// SLUS 0x80019D9C, read out of our own disassembly.
uint32_t AudioReset(SoundMachine& s) {
    GuestRam& m = s.m;
    const uint32_t gp = m.gp();
    m.W32(gp + 1912, 0xFFFFFFFFu);
    m.W32(gp + 1940, 0xFFFFFFFFu);
    m.W32(gp + 1904, 0xFFFFFFFFu);
    for (uint32_t off : {1920u, 1952u, 1924u, 1928u}) m.W32(gp + off, 0);
    m.W8(gp + 1896, 0);
    for (uint32_t off : {1900u, 1932u, 1892u, 1956u, 1916u, 1948u, 1880u, 1888u, 1960u}) m.W32(gp + off, 0);
    m.W32(gp + 1876, 1);
    for (uint32_t k = 0; k < 7; ++k) {
        const int32_t v = m.S32(0x80052638u + 4u * k);
        const uint32_t slider = U(S((U(v) << 3) - U(v)) >> 4);
        m.W32(kEngineSlider + 4u * k, slider);
        m.W32(0x800D6C20u + 4u * k, slider);
    }
    return 0;
}

// SLUS 0x8001654C, read out of our own disassembly.
void EngineSetup(SoundMachine& s) {
    GuestRam& m = s.m;
    auto recip = [](int32_t v) -> uint32_t { // 0x80000000 divu ((v >> 1) + ((v - 2) >> 31))
        const uint32_t d = U((v >> 1) + (S(U(v) - 2u) >> 31));
        return d == 0 ? 0xFFFFFFFFu : 0x80000000u / d;
    };
    uint32_t gs = m.U32(kSoundGameStatePtr);
    if (m.U32(gs + 0x30) == 0) return;
    for (uint32_t p = 0; p < m.U32(gs + 0x30); ++p) {                    // `sltu`, gs re-read below
        const uint32_t E = m.U32(m.gp() + kEngineRecGp) + kEngineRecBytes * p;
        const uint32_t bike = m.U32(kSoundPlayerBikes + 4u * p);
        m.W32(E + 0x00, bike);
        const int32_t model = m.S32(gs + 0x48 + 4u * p);
        const int32_t q = S(U(S((static_cast<int64_t>(model) * 0x38E38E39LL) >> 32) >> 1) - U(model >> 31));
        const uint32_t st = m.U32(bike + 0x22C);                         // read before the store
        m.W32(E + 0x2C, kEngineParams + 20u * U(q));
        for (uint32_t g = 0; g < 10; ++g) {
            const int32_t v = FixMul(m.S32(st + 0xBC), m.S32(st + 0x14 + 4u * g));
            if (v == 0) continue;
            const uint32_t r = recip(v);
            m.W32(kGearTable + 4u * g, U(FixMul(m.S32(st + 0x14), S(r))));
        }
        const uint32_t rb4 = recip(m.S32(st + 0xB4));
        const uint32_t P = m.U32(E + 0x2C);
        m.W32(E + 0x3C, U(FixMul(S(U(m.S32(P)) << 4), S(rb4))));
        auto divq = [](int32_t hi, int32_t lo) -> uint32_t { // d = lo + ((hi - 2) >> 31)
            const uint32_t d = U(lo + (S(U(hi) - 2u) >> 31));
            return d == 0 ? 0xFFFFFFFFu : 0x80000000u / d;
        };
        const int32_t cc = m.S32(st + 0xCC);
        m.W32(E + 0x44, divq(cc >> 1, cc >> 2));
        const int32_t cc2 = m.S32(st + 0xCC);
        m.W32(E + 0x48, divq(cc2 >> 2, cc2 >> 3));
        const int32_t w = m.S32(st + 0x190);
        const int32_t w2 = S(U(w >> 2) + U(w >> 1));
        m.W32(E + 0x50, recip(w2));
        gs = m.U32(kSoundGameStatePtr);
    }
}

// SLUS 0x800167A4 - engine_note.py m_start, line for line.
void EngineStart(SoundMachine& s, int32_t p) {
    GuestRam& m = s.m;
    const uint32_t E = EngineRec(s, p);
    if (m.U8(E + 4) != 0) return;
    const uint32_t bk = m.U32(E);
    const int32_t x = m.S32(bk + 0xB8);
    const int32_t z = m.S32(bk + 0xC0);
    int32_t vol = 0, pan = 0;
    GuestSound3DParams(s, p, x, z, 0, 0, &vol, &pan, nullptr, 0); // 0x80016804, NO doppler
    SoundParams sp40;
    sp40.pan = pan;
    const int32_t f30 = m.S32(E + 0x30);
    const int32_t f78 = m.S32(E + 0x78);
    const int32_t slider = m.S32(kEngineSlider);
    sp40.pitch = MultLo(f30, f78) >> 16;
    const int32_t f60 = m.S32(E + 0x60);
    const int32_t v56 = MultLo(vol, slider) >> 7;
    sp40.volume = MultLo(v56, f60) >> 7;
    const uint32_t h0 = GuestStartVoice(s, m.S32(E + 8), 0, 0, 1, sp40); // 0x8001687C  L0
    m.W32(E + 0x10, h0);
    sp40.volume = 0;
    const uint32_t h3 = GuestStartVoice(s, m.S32(E + 8), 3, 0, 1, sp40); // 0x8001689C  L3
    m.W32(E + 0x1C, h3);
    SetVoicePitchMod(s, h3, 1);                                          // 0x800168AC
    m.W32(E + 0x40, 128);
    sp40.pitch = 128;
    const uint32_t h4 = GuestStartVoice(s, m.S32(E + 8), 2, 0, 1, sp40); // 0x800168D0  L4
    const uint32_t h3b = m.U32(E + 0x1C);
    m.W32(E + 0x20, h4);
    if (S((h3b >> 27) - (h4 >> 27)) != 1) { // the modulator must sit on the channel just below
        StopVoice(s, h4);                                   // 0x800168F8
        StopVoice(s, m.U32(E + 0x1C));                      // 0x80016904
        SetVoicePitchMod(s, m.U32(E + 0x1C), 0);            // 0x80016910
        m.W32(E + 0x20, 0xFFFFFFFFu);
        m.W32(E + 0x1C, 0xFFFFFFFFu);
    }
    const int32_t f60b = m.S32(E + 0x60);
    sp40.pitch = -1;
    sp40.volume = MultLo(v56, 127 - f60b) >> 7;
    const uint32_t h2 = GuestStartVoice(s, m.S32(E + 8), 4, 0, 1, sp40); // 0x8001695C  L2
    const int32_t l = m.S32(E + 0x30);
    const int32_t ld = m.S32(E + 0x60);
    m.W32(E + 0x18, h2);
    for (uint32_t off : {0x14u, 0x38u, 0x68u, 0x7Cu, 0x4Cu, 0x24u, 0x28u}) m.W32(E + off, 0);
    m.W32(E + 0x54, 0xFFFFFFFFu);
    m.W32(E + 0x58, 0xFFFFFFFFu);
    m.W32(E + 0x5C, 0);
    m.W8(E + 4, 1);
    m.W32(E + 0x34, U(l));
    m.W32(E + 0x64, U(ld));
    m.W32(E + 0x70, U(v56));
    m.W32(E + 0x74, U(pan));
}

// SLUS 0x800169D0 - engine_note.py m_note, line for line.
void EngineNote(SoundMachine& s, int32_t p, uint32_t sp) {
    GuestRam& m = s.m;
    const uint32_t E = EngineRec(s, p);
    uint32_t bike = m.U32(E);
    const int32_t revs = m.S32(bike + 0x25C);
    const int32_t f3c = m.S32(E + 0x3C);
    const int32_t lvl = FixMul(revs >> 4, f3c);                          // 0x80016A14
    bike = m.U32(E);
    const uint32_t R = m.U32(bike + 0x354);
    const uint32_t rs = m.U32(R + 0x25C);
    int32_t on = 0;
    if (rs < 3) on = (m.U16(R + 0x220) != 0) ? 1 : 0;
    int32_t s2 = on ? lvl : 0;
    int32_t s1 = (rs < 2) ? -1 : 0;
    {
        const int32_t g = m.S8(bike + 0x351);
        const int32_t load = m.S32(bike + 0x24C);
        const int32_t tab = m.S32(kGearTable + 4u * U(g));
        const int32_t t = FixMul(load, tab);                              // 0x80016A80
        if ((t >> 9) < 127) {
            bike = m.U32(E);
            const int32_t g2 = m.S8(bike + 0x351);
            const int32_t load2 = m.S32(bike + 0x24C);
            const int32_t tab2 = m.S32(kGearTable + 4u * U(g2));
            s1 = S(U(s1) & U(FixMul(load2, tab2) >> 9));                  // 0x80016AB4
        } else {
            s1 = S(U(s1) & 0x7Fu);
        }
    }
    // the 3D parameters of the player's own bike, WITH the doppler (0x80016B04). The doppler slot
    // is this function's own sp+64: when the listener array is null Sound3DParams leaves it
    // unwritten and what the frame held there goes into E+0x78.
    uint32_t bk = m.U32(E);
    const int32_t vz = m.S32(bk + 0x1D0);
    const int32_t x = m.S32(bk + 0xB8);
    const int32_t z = m.S32(bk + 0xC0);
    const int32_t vx = m.S32(bk + 0x1C8);
    int32_t vol = m.S32(sp - 96u + 56u), pan = m.S32(sp - 96u + 60u), pit = m.S32(sp - 96u + 64u);
    GuestSound3DParams(s, p, x, z, vx, vz, &vol, &pan, &pit, 0);
    const int32_t slider = m.S32(kEngineSlider);
    m.W32(E + 0x74, U(pan));
    m.W32(E + 0x78, U(pit));
    m.W32(E + 0x70, U(MultLo(vol, slider) >> 7));

    constexpr int32_t kIdle = 896;
    if (s2 < kIdle) {                                                    // 0x80016B3C  IDLE
        const uint32_t f18 = m.U32(E + 0x18);
        s2 = kIdle;
        if (f18 == 0) {
            SoundParams sp40;
            const int32_t f60 = m.S32(E + 0x60);
            const int32_t f70 = m.S32(E + 0x70);
            sp40.volume = MultLo(f70, 127 - f60) >> 7;
            sp40.pan = m.S32(E + 0x74);
            const int32_t gsp = S(GetSoundPitch(s, m.S32(E + 8), 4, 0)); // 0x80016B88
            const int32_t f78 = m.S32(E + 0x78);
            sp40.pitch = MultLo(f78, gsp) >> 16;
            const uint32_t h = GuestStartVoice(s, m.S32(E + 8), 4, 0, 1, sp40); // 0x80016BBC  L2
            const uint32_t f14 = m.U32(E + 0x14);
            m.W32(E + 0x18, h);
            StopVoice(s, f14);                                           // 0x80016BC8 - even for 0
            m.W32(E + 0x14, 0);
        }
    } else {
        const uint32_t f14 = m.U32(E + 0x14);
        if (f14 == 0) {
            SoundParams sp40;
            const int32_t f60 = m.S32(E + 0x60);
            const int32_t f70 = m.S32(E + 0x70);
            sp40.volume = MultLo(f70, 127 - f60) >> 7;
            sp40.pan = m.S32(E + 0x74);
            const int32_t f30 = m.S32(E + 0x30);
            const int32_t f78 = m.S32(E + 0x78);
            sp40.pitch = MultLo(f30, f78) >> 16;
            const uint32_t h = GuestStartVoice(s, m.S32(E + 8), 1, 0, 1, sp40); // 0x80016C40  L1
            const uint32_t f18 = m.U32(E + 0x18);
            m.W32(E + 0x14, h);
            StopVoice(s, f18);                                           // 0x80016C4C
            m.W32(E + 0x18, 0);
        }
        bk = m.U32(E);
        const uint32_t surf = m.U8(bk + 0x216);
        if (!(surf - 1u < 2u)) {                                          // surface not 1 or 2
            const uint32_t r1 = GetRCnt(s, 0xF2000002u) & 0xFFu;          // 0x80016C78
            s2 = S(U(s2) + ((101u * r1) >> 8));
            const uint32_t r2 = GetRCnt(s, 0xF2000002u) & 0xFFu;          // 0x80016CA8
            s1 = S(U(s1) + ((11u * r2) >> 8));
        }
    }
    // 0x80016CCC: which sub-loop the modulator L4 and the chug layer L3 sit on
    bk = m.U32(E);
    const int32_t demand = m.S32(bk + 0x250);
    const int32_t load = m.S32(bk + 0x24C);
    int32_t sel = -1, base = 0;
    if (load < (demand >> 1)) {
        const uint32_t fl = m.U32(bk + 0x234);
        if (!(fl & 0x20u)) {
            const uint32_t P = m.U32(E + 0x2C);
            base = m.S32(P + 8);
            sel = 0;
        }
    }
    if (sel < 0) {
        const uint32_t P = m.U32(E + 0x2C);
        base = m.S32(P + 4);
        sel = 1;
    }
    m.W32(E + 0x40, U(base));
    SetVoiceLoopOffset(s, sel << 7, m.U32(E + 0x20));                    // 0x80016D2C
    bk = m.U32(E);
    const uint32_t fl = m.U32(bk + 0x234);
    const int32_t sel3 = (fl & 0x200u) ? 4 : ((fl & 0x20u) ? 2 : 0);
    SetVoiceLoopOffset(s, sel3 << 7, m.U32(E + 0x1C));                   // 0x80016D6C

    bk = m.U32(E);
    const int32_t f6c = m.S32(E + 0x6C);
    const int32_t g = m.S8(bk + 0x351);
    if (f6c < g) {
        s2 = S(U(s2) + 512u);                                             // upshift
    } else if (g < f6c) {
        s2 = S(U(s2) - 512u);                                             // downshift
        s1 = S(U(s1) + 96u);
        if (!(s1 < 127)) s1 = 126;
    }
    bk = m.U32(E);
    const uint32_t P = m.U32(E + 0x2C);
    m.W32(E + 0x6C, U(m.S8(bk + 0x351)));
    int32_t mx = m.S32(P);
    if (s2 < mx) mx = s2;
    s2 = mx;
    if (m.U8(E + 4) != 0) {                                              // 0x80016DE8
        const int32_t f30 = m.S32(E + 0x30);
        const int32_t f60 = m.S32(E + 0x60);
        m.W32(E + 0x34, U(s2));
        m.W32(E + 0x64, U(s1));
        m.W32(E + 0x38, U(S(U(s2) - U(f30)) >> 1));
        m.W32(E + 0x68, U(S(U(s1) - U(f60)) >> 1));
    } else {
        EngineStart(s, p);                                               // 0x80016E24
    }
}

// SLUS 0x80016E4C - engine_note.py m_road, line for line.
void RoadNote(SoundMachine& s, int32_t p) {
    GuestRam& m = s.m;
    const uint32_t gp = m.gp();
    int32_t s1 = 0, s3 = 0, s2 = -1, s4 = -1;
    const uint32_t E = EngineRec(s, p);
    const uint32_t bike = m.U32(E);
    const uint32_t surf = m.U8(bike + 0x216);
    const int32_t f7c = m.S32(E + 0x7C);
    const bool s5 = !(surf - 1u < 2u);
    if (f7c > 0) {                                                       // the "rider off" mode
        if (f7c & 1) {
            const int32_t spd = m.S32(bike + 0x1E0);
            s1 = (0x7FFFF < spd) ? 127 : (spd >> 12);
            const int32_t a = m.S32(bike + 0xBC);
            const int32_t b = m.S32(bike + 0x1FC);
            if (AbsW(S(U(a) - U(b))) < 16384) {
                const int32_t t = AbsW(m.S32(bike + 0x28C));
                if (s5) s2 = (t < 16385) ? 16 : 18;
                else    s2 = (t < 16385) ? 12 : 17;
            } else {
                s2 = -1;
            }
            const int32_t spd2 = m.S32(m.U32(E) + 0x1E0);
            if (!(32767 < spd2)) {
                s2 = -1;
                m.W32(E + 0x7C, m.U32(E + 0x7C) & ~1u);
            }
        }
        const uint32_t f = m.U32(E + 0x7C);
        if (f & 2u) {
            uint32_t v1 = m.U32(E);
            int32_t hard;
            if (m.U32(v1 + 0x184) & 1u) {
                hard = 1;
                v1 = m.U32(E);
            } else if (m.S8(v1 + 0x216) < 3) {
                hard = 0;
            } else {
                hard = 1;
                v1 = m.U32(E);
            }
            const uint32_t R = m.U32(v1 + 0x354);
            const int32_t spd = m.S32(R + 0x1E0);
            s3 = (0x7FFFF < spd) ? 127 : (spd >> 12);
            const uint32_t rfl = m.U32(R + 0x228);
            s4 = hard + 19;
            if (rfl & 0x40000000u) {
                const int32_t a = m.S32(R + 0xBC);
                const int32_t b = m.S32(R + 0x1FC);
                if (!(AbsW(S(U(a) - U(b))) < 16384)) s4 = -1;
            }
            if (m.S32(E + 0x58) < 0 && s4 >= 0) {
                const uint32_t b2 = m.U32(E);
                const int32_t sb = m.S8(b2 + 0x216);
                const uint32_t R2 = m.U32(b2 + 0x354);
                const int32_t rx = m.S32(R2 + 0xB8);
                const int32_t rz = m.S32(R2 + 0xC0);
                GuestPlaySound3D(s, rx, rz, sb == 4 ? 102 : 55, 0);      // 0x8001708C
            }
            const int32_t spd2 = m.S32(m.U32(m.U32(E) + 0x354) + 0x1E0);
            if (!(32767 < spd2)) {
                s4 = -1;
                m.W32(E + 0x7C, m.U32(E + 0x7C) & ~2u);
            }
        }
    } else {
        const uint32_t f238 = m.U32(bike + 0x238);
        if (f238 & 0x400u) {
            s2 = -1;
        } else {
            const uint32_t c = (m.U32(bike + 0x24) >> 25) & 3u;
            bool go = true;
            if (c == 0 && m.S32(E + 0x5C) == 0) go = false;
            if (go) {
                s2 = 17;
                if (c != 0) m.W32(E + 0x5C, 4);
                else m.W32(E + 0x5C, U(m.S32(E + 0x5C) - 1));
                s1 = Clamp127(S(U(m.S32(E + 0x5C)) << 5));
            }
            const uint32_t a0 = m.U32(E);
            const int32_t a1 = m.S32(a0 + 0x2BC);
            if (0x11FFF < a1) {
                const int32_t t = m.S32(a0 + 0x28C);
                s1 = a1 >> 10;
                if (!(AbsW(t) < 16385)) {                                 // sliding
                    s2 = 12;
                    const int32_t a1b = m.S32(a0 + 0x2A4);
                    const int32_t f44 = m.S32(E + 0x44);
                    s1 = FixMul(f44, AbsW(a1b)) >> 9;                     // 0x800171AC
                    const uint32_t r = GuestRand(m);                      // 0x800171B4
                    s1 = Clamp127(S(U(s1) - 32u + (r & 0x3Fu)));
                    if (s1 < 40) {
                        const int32_t f4c = m.S32(E + 0x4C);
                        m.W32(E + 0x4C, U(f4c + 1));
                        if (!(f4c < 8)) {
                            m.W32(E + 0x4C, 0);
                            s2 = -1;
                        }
                    } else {
                        m.W32(E + 0x4C, 0);
                    }
                    const uint32_t b2 = m.U32(E);
                    const int32_t tt = m.S32(b2 + 0x2A4);
                    const int32_t cc = m.S32(m.U32(b2 + 0x22C) + 0xCC);
                    int32_t a1c = S(U(AbsW(tt)) - U(cc >> 1));
                    s4 = 14;
                    if (a1c < 0) a1c = 0;
                    const int32_t f48 = m.S32(E + 0x48);
                    s3 = FixMul(f48, a1c) >> 9;                           // 0x80017258
                    const uint32_t r2 = GuestRand(m);                     // 0x80017260
                    s3 = Clamp127(S(U(s3) - 32u + (r2 & 0x3Fu)));
                    const int32_t f30 = m.S32(E + 0x30);
                    const int32_t f34 = m.S32(E + 0x34);
                    m.W32(E + 0x30, U(f30) + U(s3));
                    m.W32(E + 0x34, U(f34) + U(s3));
                } else {
                    s2 = 13;
                    if (!(s1 < 128)) s1 = 127;
                }
            } else {
                const int32_t v1 = m.S32(a0 + 0x2B8);
                if (0xC7FF < v1) {
                    s2 = 14;
                    s1 = Clamp127(S(U(v1) - 0xC800u) >> 7);
                }
            }
            if (s5) {
                if (m.S32(E + 0x5C) == 0 && s2 != 17) {
                    s2 = 16;
                    s1 = (s3 < s1) ? s1 : s3;
                }
                s4 = 15;
                const int32_t spd = m.S32(m.U32(E) + 0x1E0);
                s3 = (0xFFFFF < spd) ? 127 : (spd >> 14);
            }
            const uint32_t rs = m.U32(m.U32(m.U32(E) + 0x354) + 0x25C);
            if (!(rs - 1u < 3u)) s1 = 0;
        }
    }
    // ---- the tail (0x80017394)
    const int32_t t0 = m.S32(E + 0x70);
    const int32_t fxs = m.S32(kEffectsSlider);
    const int32_t mute = m.S32(gp + kRoadMuteGp);
    int32_t a3 = MultLo(t0, fxs) >> 7;
    a3 = S(U(a3) + (U(-mute) & U(-a3)));
    const int32_t roadBank = m.S32(gp + kRoadBankGp);
    int32_t sp56 = a3;
    SoundParams sp40;
    sp40.volume = MultLo(a3, s1) >> 7;
    {
        const int32_t v = S(GetSoundPitch(s, roadBank, s2, 0));          // 0x800173E4
        const int32_t f78 = m.S32(E + 0x78);
        sp40.pitch = MultLo(f78, v) >> 16;
    }
    sp40.pan = m.S32(E + 0x74);
    if (s2 != m.S32(E + 0x54)) {
        const uint32_t h = m.U32(E + 0x24);
        m.W32(E + 0x54, U(s2));
        if (h) StopVoice(s, h);                                          // 0x80017430
        if (s2 >= 0) {
            const int32_t bank = m.S32(gp + kRoadBankGp);
            m.W32(E + 0x24, GuestStartVoice(s, bank, s2, 0, 1, sp40));    // 0x80017450
        }
    } else {
        const uint32_t h = m.U32(E + 0x24);
        if (h) UpdateVoice(s, h, sp40);                                  // 0x80017470
    }
    if (m.S32(E + 0x7C) != 0) {                                          // B follows the rider
        const uint32_t R = m.U32(m.U32(E) + 0x354);
        const int32_t rx = m.S32(R + 0xB8);
        const int32_t rz = m.S32(R + 0xC0);
        int32_t vol = sp56, pan = 0;
        GuestSound3DParams(s, p, rx, rz, 0, 0, &vol, &pan, nullptr, 0);  // 0x800174B8
        const int32_t fx2 = m.S32(kEffectsSlider);
        sp40.pan = pan;
        const int32_t mute2 = m.S32(gp + kRoadMuteGp);
        const int32_t w = MultLo(vol, fx2) >> 7;
        sp56 = S(U(w) + (U(-mute2) & U(-w)));
    }
    const int32_t bankB = m.S32(gp + kRoadBankGp);
    sp40.volume = MultLo(sp56, s3) >> 7;
    {
        const int32_t v = S(GetSoundPitch(s, bankB, s4, 0));             // 0x80017524
        const int32_t f78 = m.S32(E + 0x78);
        sp40.pitch = MultLo(f78, v) >> 16;
    }
    if (s4 != m.S32(E + 0x58)) {
        const uint32_t h = m.U32(E + 0x28);
        m.W32(E + 0x58, U(s4));
        if (h) StopVoice(s, h);                                          // 0x80017564
        if (s4 >= 0) {
            const int32_t bank = m.S32(gp + kRoadBankGp);
            m.W32(E + 0x28, GuestStartVoice(s, bank, s4, 0, 1, sp40));    // 0x80017584
        }
    } else {
        const uint32_t h = m.U32(E + 0x28);
        if (h) UpdateVoice(s, h, sp40);                                  // 0x800175A4
    }

    // ---- visual effects, not sound (0x800175AC..0x80017670)
    uint32_t b = m.U32(E);
    if (m.U32(b + 0x234) & 0x18000000u) {
        if (s.fx) s.fx->Burst(b, 1, 600, 0);                             // 0x800175CC
        else s.Fail("RoadNote needs the effect spawner 0x80027778", b);
        b = m.U32(E);
    } else {
        bool spawned = false;
        if (m.U32(b + 0x184) & 1u) {
            if (!(m.S16(b + 0x1E2) < 13)) {
                if (s.fx) s.fx->Spray(b, b + 0xB8u, 0);                  // 0x80017634
                else s.Fail("RoadNote needs the effect spawner 0x80027974", b);
                b = m.U32(E);
                spawned = true;
            }
        }
        if (!spawned) {
            b = m.U32(E);
            if (0x11FFF < m.S32(b + 0x2BC)) {
                if (m.U32(b + 0x234) & 8u) {
                    if (s.fx) s.fx->Spray(b, b + 0xB8u, 0);              // 0x80017634
                    else s.Fail("RoadNote needs the effect spawner 0x80027974", b);
                    b = m.U32(E);
                }
            }
        }
    }
    if (0x11FFF < m.S32(b + 0x2B8)) {
        if (m.U32(b + 0x234) & 4u) {
            if (s.fx) s.fx->Spray(b, b + 0xB8u, 2);                      // 0x8001766C
            else s.Fail("RoadNote needs the effect spawner 0x80027974", b);
        }
    }
}

} // namespace rr::sim
