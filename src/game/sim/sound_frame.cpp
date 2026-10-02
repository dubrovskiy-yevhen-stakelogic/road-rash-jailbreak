// AudioFrame and the object-sound layer under it - see sound_frame.h. Transcribed from our own
// disassembly; every load, store and call is in the original's order where a store can alias a
// later load (the slot arrays, the listener records and the cue slots are all re-read after every
// callee, exactly as the original re-reads them through `gp`).
#include "game/sim/sound_frame.h"

namespace rr::sim {
namespace {

constexpr uint32_t kRamBytes = 2u * 1024u * 1024u;

int32_t S(uint32_t v) { return static_cast<int32_t>(v); }
uint32_t U(int32_t v) { return static_cast<uint32_t>(v); }
int32_t MultLo(int32_t a, int32_t b) { return S(U(a) * U(b)); }
uint32_t Sllv(uint32_t v, uint32_t n) { return v << (n & 31u); }
int32_t AbsW(int32_t x) { // `sra 31 / addu / xor`
    const int32_t m = x >> 31;
    return S((U(x) + U(m)) ^ U(m));
}
int32_t Half(int32_t v) { return S(U(v) + (U(v) >> 31)) >> 1; } // `srl 31 / addu / sra 1`

bool IsRam(uint32_t a) {
    if (a >= 0xC0000000u) return false;
    return (a & 0x1FFFFFFFu) < 0x00800000u;
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

uint32_t Listener(SoundMachine& s, int32_t p) { return s.m.U32(s.m.gp() + kListenerGp) + kListenerBytes * U(p); }
uint32_t Slots(SoundMachine& s, int32_t p) { return s.m.U32(s.m.gp() + kObjSlotsGp + 4u * U(p)); }
uint32_t GameState(SoundMachine& s) { return s.m.U32(kSoundGameStatePtr); }

// The entity an object handle names: class = handle >> 5, index = handle & 31, through the pool
// table 0x800CE4D0 - except class 5 index 29.., which are the 280-byte police records
// (`*(0x8005B304) + index * 280 - 8120`, 0x80019400 / 0x80018688).
uint32_t EntityOf(SoundMachine& s, uint32_t handle) {
    const uint32_t cls = (handle & 0xFFFFu) >> 5;
    const uint32_t idx = handle & 31u;
    if (cls == 5 && !(idx < 29)) return s.m.U32(kCopRecordsPtr) + idx * 280u - 8120u;
    const uint32_t t = 0x800CE4D0u + (cls << 4);
    return s.m.U32(t) + s.m.U32(t + 4) * idx;
}

// A SoundBytes view of the listener array (four records, as the ported emitter views it).
SoundBytes ListenerView(SoundMachine& s) {
    const uint32_t L = s.m.U32(s.m.gp() + kListenerGp);
    if (L == 0 || !IsRam(L)) return SoundBytes();
    const uint32_t off = L & (kRamBytes - 1u);
    uint32_t n = 4u * kListenerBytes;
    if (off + n > kRamBytes) n = kRamBytes - off;
    return SoundBytes(s.ram + off, n);
}

// The slider of an object slot's kind (0x80017754..0x8001777C): kinds 3..5 use slider 2, kind 2
// slider 4, every other slider 1.
uint32_t KindSlider(int32_t kind) {
    if (U(kind - 3) < 3u) return 2;
    return (kind == 2) ? 4u : 1u;
}

} // namespace

// ============================================================================ the loading side
void ResetVoiceLists(SoundMachine& s) {
    GuestRam& m = s.m;
    for (int32_t i = 21; i >= 0; --i) m.W32(kSoundSystem + 0x58 + 4u * U(i), 0xFFFFFFFFu);
    for (int32_t i = 23; i >= 0; --i) m.W32(kSoundSystem + 0xB0 + 4u * U(i), U(i));
    for (int32_t i = 24; i >= 0; --i) m.W32(kSoundSystem + 0x110 + 4u * U(i), 0xFFFFFFFFu);
    m.W32(kSoundSystem + 0x174, 23);
    m.W32(kSoundSystem + 0x178, 0);
    m.W32(kSoundSystem + 0x17C, 0);
}

void ResetSoundState(SoundMachine& s) {
    GuestRam& m = s.m;
    for (uint32_t off = 0; off <= 0x18; off += 4) m.W32(kSoundSystem + off, 0);
    for (int32_t k = 2; k >= 0; --k) m.W32(kSoundSystem + 0x1C + 20u * U(k), 0); // 0x8001E5DC
    m.W32(kSoundSystem + 0x180, 127);
    m.W32(kSoundSystem + 0x184, 0);
    ResetVoiceLists(s);
}

void PatchBank(SoundMachine& s, uint32_t bank, uint32_t base) {
    GuestRam& m = s.m;
    if (m.U8(bank + 4) == 0) return;
    for (uint32_t i = 0; S(i) < S(m.U8(bank + 4)); ++i) {                 // `lbu` re-read, 0x8001EB0C
        const uint32_t o = m.U32(bank + 16u + 4u * i);
        if (o == 0) continue;
        const uint32_t rec = bank + o;
        if (m.U8(rec) == 0) continue;
        uint32_t a = rec + 12u;
        for (int32_t d = 0; d < S(m.U8(rec)); ++d, a += 12u)             // re-read after each store
            m.W32(a, m.U32(a) + base);
        if (m.Faulted()) return;
    }
}

uint32_t ReleaseAllVoices(SoundMachine& s) {
    GuestRam& m = s.m;
    int32_t n = m.S32(kSoundSystem + 0x08);
    uint32_t v = m.U32(kSoundSystem + 0x0C);
    uint32_t mask = 0;
    if (n < 0 || n > 64) { // `bnez` counts a negative count down through 2^32 records
        s.Fail("ReleaseAllVoices with a voice count outside 0..64", U(n));
        return 0;
    }
    for (; n != 0; --n, v += 44u) {
        if (m.U32(v + 4) == 0) continue;
        mask |= Sllv(1u, m.U32(v + 0x1C));
        ReleaseVoice(s, v);
        if (s.Faulted()) return 0;
    }
    m.W32(kSoundSystem + 0x18, m.U32(kSoundSystem + 0x18) | mask); // 0x8001EB60
    return SoundService(s);
}

void SoundRecordsInit(SoundMachine& s, int32_t p) {
    GuestRam& m = s.m;
    const uint32_t E = m.U32(0x8005B42Cu) + 132u * U(p);
    const uint32_t L = m.U32(0x8005B40Cu);
    for (uint32_t off : {0x10u, 0x14u, 0x18u, 0x24u, 0x28u, 0x08u}) m.W32(E + off, 0xFFFFFFFFu);
    const uint32_t Lp = L + kListenerBytes * U(p);
    m.W32(Lp + 0x1C, 0xFFFFFFFFu);
    const uint32_t arr = 0x8005B410u + 4u * U(p);
    for (int32_t i = 0; i < S(m.U32(Lp + 0x24) + m.U32(Lp + 0x28)); ++i) {
        m.W16(m.U32(arr) + kObjSlotBytes * U(i), 0xE0);
        if (m.Faulted()) return;
    }
}

// ============================================================================ libspu
uint32_t SetReverbDepth(SoundMachine& s, uint32_t left, uint32_t right) {
    // 0x8001F054 builds {mask 0, ..., +8 left, +10 right} and calls 0x800505F8: mask 0 sets both.
    GuestRam& m = s.m;
    Store16(s, m.U32(kSpuBasePtr) + 388u, left & 0xFFFFu);
    m.W16(kSpuReverbDepthL, static_cast<uint16_t>(left));
    Store16(s, m.U32(kSpuBasePtr) + 390u, right & 0xFFFFu);
    m.W16(kSpuReverbDepthL + 2u, static_cast<uint16_t>(right));
    return 0;
}

// ============================================================================ the cue voice
void SpeechCue(SoundMachine& s, int32_t kind) {
    GuestRam& m = s.m;
    int32_t bank = 0, sound = 0;
    if (kind == 4) {
        if (m.S32(0x800D6C58u) != 23) return;
        if (m.S32(kCueSlots + 0xC8u) != 2) return;
        sound = 0;
        bank = m.S32(kCueSlots + 0xC0u);
        m.W32(kCueHandle + 0x0C, 6);
        m.W32(kCueSlots + 0xC8u, 3);
        m.W32(0x800D6C58u, 4);
    } else {
        sound = kind;
        bank = m.S32(m.gp() + kCueBankGp);
        m.W32(kCueHandle + 0x0C, 0xFFFFFFFFu);
    }
    if (kind == 3 && m.U32(kCueHandle + 0x10) != 0) return;
    if (m.U32(kCueHandle + 0x08) != 0) {
        const uint32_t h = m.U32(kCueHandle);
        if (h != 0) StopVoice(s, h);
        SoundParams sp;
        sp.pan = 64;
        sp.pitch = -1;
        const int32_t v = m.S32(kEffectsSlider);
        sp.volume = S((U(v) << 7) - U(v)) >> 7;
        const uint32_t nh = GuestStartVoice(s, bank, sound, 1, 1, sp);   // 0x8001B354
        const uint32_t gs = GameState(s);
        m.W32(kCueHandle, nh);
        const uint32_t clock = m.U32(gs + 0x10);
        m.W32(kCueHandle + 0x08, 0);
        m.W32(kCueHandle + 0x04, clock + 1500u);
    }
    if (kind == 3) {
        const uint32_t gs = GameState(s);
        m.W32(kCueHandle + 0x10, 1);
        const uint32_t clock = m.U32(gs + 0x10);
        m.W32(kCueHandle + 0x08, 1);
        m.W32(kCueHandle + 0x14, clock + 9000u);
    }
}

// ============================================================================ the spatial query
void SpatialQuery(SoundMachine& s, uint32_t hits, uint32_t count, uint32_t pos, int32_t reach,
                  uint32_t classMask, uint32_t self) {
    GuestRam& m = s.m;
    const uint32_t me = self & 0xFFFFu;
    int32_t t0 = 0;
    {
        const int32_t z = m.S32(pos + 8), x = m.S32(pos + 0);
        const int32_t xc = S(U(x) - m.U32(kGridOriginX)) >> 21;
        const int32_t zc0 = S(U(z) - m.U32(kGridOriginZ)) >> 21;
        const int32_t a0 = xc + 11;
        int32_t a2 = zc0 + 11;
        const uint32_t gs = GameState(s);
        if (m.U32(gs + 0x30) != 1) {
            if (!(U(a0) < 24u && U(a2) < 24u)) {
                a2 = (S(U(z) - m.U32(kGridOriginZ + 4)) >> 21) + 11;
                a2 = S(U(a2) + Sllv(24u, U(a2 >> 31)));
            }
        }
        const uint32_t hiGrid = (a2 < 24) ? 0u : 1u;
        const int32_t half = S(U(reach) + U(Half(reach)));
        const uint32_t np = m.U32(GameState(s) + 0x30);
        const uint32_t grid = hiGrid & ((a2 < S(Sllv(24u, np - 1u))) ? 1u : 0u);
        const int32_t ox = m.S32(kGridOriginX + 4u * grid);
        const int32_t oz = m.S32(kGridOriginZ + 4u * grid);
        const int32_t px = m.S32(pos + 0), pz = m.S32(pos + 8);
        int32_t x0 = (S(U(px) - U(half) - U(ox)) >> 21) + 11;
        int32_t x1 = (S(U(px) + U(half) - U(ox)) >> 21) + 11;
        int32_t z0 = (S(U(pz) - U(half) - U(oz)) >> 21) + 11;
        int32_t z1 = (S(U(pz) + U(half) - U(oz)) >> 21) + 11;
        if (x0 < 0) x0 = 0;
        if (z0 < 0) z0 = 0;
        if (!(x1 < 24)) x1 = 23;
        if (!(z1 < 24)) z1 = 23;
        if (!(x0 < 24) || !(z0 < 24) || x1 < 0 || z1 < 0) {
            m.W32(count, 0);
            return;
        }
        const uint32_t row0 = U(-S(grid)) & 0x18u;
        for (int32_t zr = z0; !(z1 < zr); ++zr) {
            const uint32_t pool0 = m.U32(kRoadPool0Ptr);                  // 0x8008B07C, once per row
            for (int32_t xr = x0; !(x1 < xr); ++xr) {
                uint32_t node = m.U8(kGridCells + U(xr) + (row0 + U(zr)) * 24u);
                if (node == 0x80) continue;
                const uint32_t pool1 = m.U32(kPool1Ptr);                  // 0x8008B0B4, once per cell
                while (node != 0x80) {
                    const uint32_t id = m.U8(kGridNodes + 2u * node + 1u);
                    const uint32_t cls = id >> 5;
                    if (classMask & Sllv(1u, cls)) {
                        uint32_t h = 0, xAt = 0, zAt = 0, hAt = 0;
                        bool known = true;
                        if (cls == 0) {
                            const uint32_t e = pool0 + (id & 0xFFFFu) * 1096u;
                            hAt = e + 0xAC; xAt = e + 0xB8; zAt = e + 0xC0;
                        } else if (cls == 1) {
                            const uint32_t e = pool1 + (id & 31u) * 628u;
                            hAt = e + 0xAC; xAt = e + 0xB8; zAt = e + 0xC0;
                        } else {
                            const uint32_t t = 0x800CE4D0u + (cls << 4);
                            const uint32_t rec = m.U32(t) + m.U32(t + 4) * (id & 31u) +
                                                 m.U32(kGridPoolOffsets + 4u * (cls - 2u));
                            hAt = rec; xAt = rec + 0xC; zAt = rec + 0x14;
                            known = false; // the handle is kept, not re-read (0x8008B3A8)
                        }
                        h = m.U16(hAt);
                        if (h != me) {
                            const int32_t a = AbsW(S(m.U32(xAt) - m.U32(pos + 0)));
                            const int32_t b = AbsW(S(m.U32(zAt) - m.U32(pos + 8)));
                            const int32_t sum = S(U(a) + U(b));
                            const int32_t mn = (a < b) ? a : b;
                            if (!(reach < S(U(sum) - U(Half(mn))))) {
                                int32_t k = 0;
                                if (t0 != 0) {
                                    for (k = 0; k != t0; ++k)
                                        if (m.U16(hits + 2u * U(k)) == h) break;
                                }
                                if (k == t0) {
                                    const uint32_t put = known ? m.U16(hAt) : h;
                                    m.W16(hits + 2u * U(t0), static_cast<uint16_t>(put));
                                    const int32_t cap = m.S32(count);
                                    ++t0;
                                    if (t0 == cap) return;                 // 0x8008B3B8, count unwritten
                                }
                            }
                        }
                    }
                    node = m.U8(kGridNodes + 2u * node);
                    if (m.Faulted()) return;
                }
            }
        }
    }
    m.W32(count, U(t0));
}

// ============================================================================ the object slots
void ObjSoundStart(SoundMachine& s, int32_t slot, int32_t p, int32_t bank, int32_t sound, uint32_t sp) {
    GuestRam& m = s.m;
    const uint32_t base = Slots(s, p);
    const uint32_t e = base + kObjSlotBytes * U(slot);
    if (base == 0) return;                                                // 0x800176E4 tests the base
    const int32_t x = S(m.U32(e + 0x0C) + U(m.S32(e + 0x14) >> 6));
    m.W32(e + 0x0C, U(x));
    const int32_t z = S(m.U32(e + 0x10) + U(m.S32(e + 0x18) >> 6));
    m.W32(e + 0x10, U(z));
    const int32_t shift = (m.S32(e + 0x08) == 6) ? 3 : 0;
    const uint32_t fr = sp - 88u;
    int32_t vol = m.S32(fr + 56), pan = m.S32(fr + 60), pit = m.S32(fr + 64);
    GuestSound3DParams(s, p, x, z, m.S32(e + 0x14), m.S32(e + 0x18), &vol, &pan, &pit, shift);
    const int32_t slider = m.S32(kEngineSlider + 4u * KindSlider(m.S32(e + 0x08)));
    SoundParams prm;
    prm.pan = pan;
    prm.volume = MultLo(vol, slider) >> 7;
    prm.pitch = MultLo(m.S32(e + 0x1C), pit) >> 16;
    const uint32_t h = GuestStartVoice(s, bank, sound, 0, 1, prm);         // 0x800177E8
    m.W32(e + 0x20, h);
    m.W32(e + 0x04, 1);
}

void ObjSoundStop(SoundMachine& s, int32_t slot, int32_t p) {
    GuestRam& m = s.m;
    const uint32_t arr = m.gp() + kObjSlotsGp + 4u * U(p);
    const uint32_t e = m.U32(arr) + kObjSlotBytes * U(slot);
    if (e == 0) return;
    if (m.U32(e + 0x04) != 0) {
        StopVoice(s, m.U32(e + 0x20));                                     // 0x8001787C
        const uint32_t L = m.U32(m.gp() + kListenerGp) + kListenerBytes * U(p);
        const int32_t n = m.S32(L + 0x24);
        if (slot < n) {
            uint32_t o = m.U32(arr) + kObjSlotBytes * U(n);
            if (m.S32(L + 0x28) > 0) {
                for (int32_t k = 0;;) {
                    if (m.U16(e) == m.U16(o) && !(m.S32(o + 0x08) < 3)) {
                        const uint32_t Lk = m.U32(m.gp() + kListenerGp) + kListenerBytes * U(p);
                        ObjSoundStop(s, m.S32(Lk + 0x24) + k, p);
                    }
                    ++k;
                    const uint32_t L2 = m.U32(m.gp() + kListenerGp) + kListenerBytes * U(p);
                    if (!(k < m.S32(L2 + 0x28))) break;
                    o += kObjSlotBytes;
                    if (s.Faulted()) return;
                }
            }
        }
        if (m.S32(e + 0x08) == 2) {
            const int32_t cue = m.S32(e + 0x28);
            const uint32_t rec = kCueSlots + (U(cue) << 5);
            if (m.S32(rec + 0x08) == 3 || cue < 0) {
                m.W32(rec + 0x08, 4);
                m.W32(m.U32(m.gp() + kListenerGp) + kListenerBytes * U(p) + 0x2C, 0);
            }
        }
    }
    const uint32_t clock = m.U32(GameState(s) + 0x0C);
    m.W16(e, 0xE0);
    m.W32(e + 0x08, 0);
    m.W32(e + 0x04, 0);
    m.W32(e + 0x24, clock);
}

void ObjSoundUpdate(SoundMachine& s, int32_t slot, int32_t p, uint32_t sp) {
    GuestRam& m = s.m;
    const uint32_t e = Slots(s, p) + kObjSlotBytes * U(slot);
    const int32_t kind = m.S32(e + 0x08);
    int32_t shift = (kind == 6) ? 2 : 0;
    const int32_t x = S(m.U32(e + 0x0C) + U(m.S32(e + 0x14) >> 6));
    m.W32(e + 0x0C, U(x));
    const int32_t z = S(m.U32(e + 0x10) + U(m.S32(e + 0x18) >> 6));
    m.W32(e + 0x10, U(z));
    if (U(kind - 3) < 2u) shift = 2;
    const uint32_t fr = sp - 80u;
    int32_t vol = m.S32(fr + 56), pan = m.S32(fr + 60), pit = m.S32(fr + 64);
    GuestSound3DParams(s, p, x, z, m.S32(e + 0x14), m.S32(e + 0x18), &vol, &pan, &pit, shift);
    const int32_t slider = m.S32(kEngineSlider + 4u * KindSlider(m.S32(e + 0x08)));
    vol = MultLo(vol, slider) >> 7;
    if (m.U32(m.gp() + kRoadMuteGp) != 0) vol = 0;
    SoundParams prm;
    prm.volume = vol;
    prm.pan = pan;
    prm.pitch = MultLo(m.S32(e + 0x1C), pit) >> 16;
    UpdateVoice(s, m.U32(e + 0x20), prm);                                  // 0x80017B18 -> 0x8001F6A4
}

void ObjSoundClaim(SoundMachine& s, uint32_t handle, int32_t p, uint32_t sp) {
    GuestRam& m = s.m;
    const uint32_t id = handle & 0xFFFFu; // the callers pass a `lhu`
    const uint32_t arr = m.gp() + kObjSlotsGp + 4u * U(p);
    const uint32_t base = m.U32(arr);
    if (base == 0) return;
    int32_t busy = 0;
    const uint32_t L = m.U32(m.gp() + kListenerGp) + kListenerBytes * U(p);
    const int32_t first = m.S32(L + 0x24);
    const int32_t end = S(U(first) + m.U32(L + 0x28));
    {
        uint32_t e = base + kObjSlotBytes * U(first);
        for (int32_t i = first; i < end; ++i, e += kObjSlotBytes)
            if (m.U16(e) == id && !(m.S32(e + 0x08) < 3) && m.U32(e + 0x04) != 0) return;
    }
    int32_t at = first;
    bool found = false;
    uint32_t e = m.U32(arr) + kObjSlotBytes * U(m.S32(m.U32(m.gp() + kListenerGp) + kListenerBytes * U(p) + 0x24));
    if (at < end) {
        const uint32_t gs = GameState(s);
        for (; at < end; ++at, e += kObjSlotBytes) {
            if (m.U32(e + 0x04) == 0) {
                found = true;
                break;
            }
            if (m.U8(gs + 4) == 33 && !(m.S32(e + 0x08) < 3)) {
                ++busy;
                if (!(busy < 2)) return;
            }
        }
    }
    if (!found) {
        at = m.S32(m.U32(m.gp() + kListenerGp) + kListenerBytes * U(p) + 0x24);
        e = m.U32(arr) + kObjSlotBytes * U(at);
    }
    if (m.U32(e + 0x04) != 0) ObjSoundStop(s, at, p);                       // 0x80018168
    // Always through the pool table - no police arm here, unlike AudioFrame (0x80018170).
    const uint32_t t = 0x800CE4D0u + ((id >> 5) << 4);
    const uint32_t ent = m.U32(t) + m.U32(t + 4) * (id & 31u);
    m.W16(e, static_cast<uint16_t>(id));
    m.W32(e + 0x0C, m.U32(ent + 0xB8));
    m.W32(e + 0x10, m.U32(ent + 0xC0));
    m.W32(e + 0x14, U(MultLo(m.S16(ent + 0x1C2), m.S32(ent + 0x1E0) >> 4) >> 8));
    const int32_t vz = MultLo(m.S16(ent + 0x1C6), m.S32(ent + 0x1E0) >> 4) >> 8;
    m.W32(e + 0x18, U(vz));
    const uint32_t pitch = GetSoundPitch(s, m.S32(m.gp() + kRoadBankGp), 11, 0);
    m.W32(e + 0x1C, pitch);
    m.W32(e + 0x08, (m.U32(ent + 0xB4) == 0) ? 5u : (id & 1u) + 3u);
    ObjSoundStart(s, at, p, m.S32(m.gp() + kRoadBankGp), 11, sp - 40u);     // 0x80018238
}

void ObjSoundPick(SoundMachine& s, int32_t p, uint32_t sp) {
    GuestRam& m = s.m;
    const uint32_t gp = m.gp();
    const uint32_t fr = sp - 184u;
    const uint32_t hits = fr + 80u, cnt = fr + 136u, dist = fr + 24u, order = fr + 112u;
    m.W32(cnt, 10);
    const uint32_t bike = m.U32(m.U32(gp + kEngineRecGp) + kEngineRecBytes * U(p));
    const uint32_t self = m.U16(bike + 0xAC);
    SpatialQuery(s, hits, cnt, kViewRecords + 0xB8u + kViewRecordBytes * U(p), 0x800000, 9, self); // 0x80018540
    if (s.Faulted()) return;
    if (m.U32(kCopRecordsOn) != 0) {
        for (uint32_t k = 0; k < 3; ++k) {
            const uint32_t rec = m.U32(kCopRecordsPtr) + 280u * k;
            const uint32_t h = m.U16(rec + 0xAC);
            if (h == 0 || m.S8(rec + 8) == 9) continue;
            const uint32_t c = m.U32(cnt);
            m.W16(hits + 2u * c, static_cast<uint16_t>(h));
            m.W32(cnt, c + 1u);
        }
    }
    for (int32_t k = 12; k >= 0; --k) m.W32(dist + 4u * U(k), 0x7FFFFFFFu);
    {
        const uint32_t L = m.U32(gp + kListenerGp) + kListenerBytes * U(p);
        for (int32_t k = 0; k < m.S32(L + 0x24); ++k) m.W32(order + 4u * U(k), 0xFFFFFFFFu);
    }
    // the nearest first: an insertion sort of the hits into `order`, octagonal distances
    for (int32_t i = 0; i < m.S32(cnt); ++i) {
        const uint32_t ent = EntityOf(s, m.U16(hits + 2u * U(i)));
        const uint32_t L = m.U32(gp + kListenerGp) + kListenerBytes * U(p);
        int32_t a = AbsW(S(m.U32(ent + 0xB8) - m.U32(L + 4)));
        int32_t b = AbsW(S(m.U32(ent + 0xC0) - m.U32(L + 8)));
        if (a < b) {
            const int32_t t = a;
            a = b;
            b = t;
        }
        const int32_t b3 = b + (b >> 1);
        const int32_t d = S(U(S(U(a) - U(a >> 5)) - U(a >> 7)) + U(b3 >> 2) + U(b3 >> 6));
        m.W32(dist + 4u * U(i), U(d));
        const uint32_t gs = GameState(s);
        const uint32_t f = m.U8(gs + 4);
        if ((f & 1u) && f != 33) {
            const uint32_t pb = m.U32(kSoundPlayerBikes + 4u * U(p));
            if ((m.U8(m.U32(pb + 0x43C) + 1) & 0xFu) == 2 && m.U16(ent + 0xAC) == m.U8(gs + 6)) {
                const int32_t gap = AbsW(S(m.U32(ent + 0x144) - m.U32(pb + 0x144)));
                if (!(0x31FFF < gap)) SpeechCue(s, 3);                     // 0x800187E4
            }
        }
        const uint32_t L2 = m.U32(gp + kListenerGp) + kListenerBytes * U(p);
        for (int32_t k = 0; k < m.S32(L2 + 0x24); ++k) {
            const int32_t o = m.S32(order + 4u * U(k));
            if (o >= 0) {
                if (!(d < m.S32(dist + 4u * U(o)))) continue;
                for (int32_t j = m.S32(L2 + 0x24) - 1; k < j; --j)
                    m.W32(order + 4u * U(j), m.U32(order + 4u * U(j - 1)));
            }
            m.W32(order + 4u * U(k), U(i));
            break;
        }
        if (s.Faulted()) return;
    }
    // free the slots whose object is no longer among the nearest
    for (int32_t k = 0; k < m.S32(m.U32(gp + kListenerGp) + kListenerBytes * U(p) + 0x24); ++k) {
        const uint32_t id = m.U16(Slots(s, p) + kObjSlotBytes * U(k));
        if (id == 0xE0) continue;
        bool found = false;
        const int32_t n = m.S32(m.U32(gp + kListenerGp) + kListenerBytes * U(p) + 0x24);
        for (int32_t j = 0; j < n; ++j) {
            const int32_t o = m.S32(order + 4u * U(j));
            if (o >= 0 && id == m.U16(hits + 2u * U(o))) {
                found = true;
                break;
            }
        }
        if (!found) ObjSoundStop(s, k, p);                                 // 0x80018978
        if (s.Faulted()) return;
    }
    // give each new one a free slot
    {
        auto nObj = [&]() { return m.S32(m.U32(gp + kListenerGp) + kListenerBytes * U(p) + 0x24); };
        if (nObj() > 0) {
            const int32_t total = m.S32(cnt);
            for (int32_t k = 0; k < nObj(); ++k) {
                const uint32_t L = m.U32(gp + kListenerGp) + kListenerBytes * U(p);
                const int32_t o = m.S32(order + 4u * U(k));
                if (o < 0) continue;
                bool found = false;
                if (total > 0) {
                    const uint32_t want = m.U16(hits + 2u * U(o));
                    uint32_t e = Slots(s, p);
                    for (int32_t j = 0; j < total; ++j, e += kObjSlotBytes)
                        if (want == m.U16(e)) {
                            found = true;
                            break;
                        }
                }
                if (found) continue;
                for (int32_t j = 0; j < m.S32(L + 0x24); ++j) {
                    const uint32_t e = Slots(s, p) + kObjSlotBytes * U(j);
                    if (m.U16(e) != 0xE0) continue;
                    m.W16(e, m.U16(hits + 2u * m.U32(order + 4u * U(k))));
                    m.W32(Slots(s, p) + kObjSlotBytes * U(j) + 0x08, 1);
                    break;
                }
            }
        }
    }
    // no object on two slots
    for (int32_t k = 0; k < m.S32(m.U32(gp + kListenerGp) + kListenerBytes * U(p) + 0x24); ++k) {
        const uint32_t id = m.U16(Slots(s, p) + kObjSlotBytes * U(k));
        if (id == 0xE0) continue;
        for (int32_t j = k + 1; j < m.S32(m.U32(gp + kListenerGp) + kListenerBytes * U(p) + 0x24); ++j) {
            if (id == m.U16(Slots(s, p) + kObjSlotBytes * U(j))) ObjSoundStop(s, j, p); // 0x80018BA0
            if (s.Faulted()) return;
        }
    }
}

// ============================================================================ the music
uint32_t MusicPickShuffle(SoundMachine& s) {
    GuestRam& m = s.m;
    auto flagsAt = [](uint32_t i) { return kAlbumTable + 16u + 12u * i; };
    uint32_t selectable = 0, unplayed = 0;
    const uint32_t n0 = m.U32(kAlbumTable);
    for (uint32_t i = 0; i < n0; ++i) {
        const uint32_t f = m.U32(flagsAt(i));
        const uint32_t sel = f & 1u;
        selectable += sel;
        if (sel) unplayed += ((f & 2u) == 0) ? 1u : 0u;
        if (m.Faulted()) return 0;
    }
    if (selectable == 0) return 0xFFFFFFFFu;
    if (unplayed == 0 && m.U32(kAlbumTable) != 0)                           // a new shuffle cycle
        for (uint32_t i = 0; i < m.U32(kAlbumTable); ++i) m.W32(flagsAt(i), m.U32(flagsAt(i)) & ~2u);
    const uint32_t r = GetRCnt(s, 0xF2000002u);                             // 0x80024BD0
    uint32_t at = (m.U32(kAlbumTable) * (r & 0xFFu)) >> 8;
    uint32_t tries = 0;
    uint32_t f = m.U32(flagsAt(at));
    while (!(f & 1u) || (f & 2u)) {
        const uint32_t n = m.U32(kAlbumTable);
        if (!(tries < n)) return 0xFFFFFFFFu;
        ++at;
        ++tries;
        if (!(at < n)) at = 0;
        f = m.U32(flagsAt(at));
        if (m.Faulted()) return 0;
    }
    if (!(tries < m.U32(kAlbumTable))) return 0xFFFFFFFFu;
    m.W32(flagsAt(at), m.U32(flagsAt(at)) | 2u);
    return at;
}

uint32_t StreamVoiceStart(SoundMachine& s, int32_t adsrVariant, const SoundParams& p, uint32_t spuAddr) {
    GuestRam& m = s.m;
    int32_t left = 0, right = 0;
    if (m.U32(kSoundSystem + 0x184) == 0) {                                 // panning on
        const int32_t pan = p.pan;
        const bool front = pan < 129;
        const int32_t folded = front ? pan : 256 - pan;
        const int32_t V = S(U(p.volume) << 7);
        const int32_t prod = MultLo(folded - 64, V);
        const int32_t P = prod >> 6;
        const int32_t pos = S(U(P) & U(S(U(0) - U(P)) >> 31));                // max(0, P), as `negu/sra/and`
        int32_t l = S(U(S(U(V) - U(pos))) ^ U(pan >> 31));
        if (!front) l = S(U(0) - U(l));
        left = l;
        right = S(U(V) + (U(P) & U(prod >> 31)));
    } else {
        left = right = S(U(p.volume) << 7);
    }
    const uint32_t v = GuestAllocVoice(s, 1);                               // 0x8001F430, RESERVED
    if (v == 0) return 0;
    m.W32(v + 0x08, 0);
    const uint32_t handle = (m.U32(v + 0x1C) << 27) + (m.U32(v + 0x04) & 0x07FFFFFFu);
    for (uint32_t k = 0; k < 3; ++k) {                                      // 0x8001F488..0x8001F4D4
        const uint32_t rec = kSoundSystem + 20u * k;                        // the record is rec+0x1C
        if (m.U32(rec + 0x1C) != 0) continue;
        m.W32(rec + 0x1C, 1);
        m.W32(rec + 0x2C, handle);
        m.W32(rec + 0x28, spuAddr);
        m.W8(rec + 0x20, 0x7F);
        m.W8(rec + 0x21, 0x7F);
        m.W16(rec + 0x22, static_cast<uint16_t>(adsrVariant != 0 ? 0xB9FFu : 0x98FFu));
        m.W16(rec + 0x24, 0x5FCE);
        m.W32(v + 0x08, rec + 0x20);                                        // the descriptor
        break;
    }
    if (m.U32(v + 0x08) == 0) {                                             // every record busy
        ReleaseVoice(s, v);
        return 0;
    }
    m.W32(v + 0x20, U(p.pitch));
    m.W32(v + 0x24, U(left));
    m.W32(v + 0x28, U(right));
    SpuSetReverbVoice(s, 0, Sllv(1u, m.U32(v + 0x1C)));                      // 0x8001F514
    return handle;
}

uint32_t KeyOnHandles(SoundMachine& s, int32_t n, uint32_t handles) {
    GuestRam& m = s.m;
    uint32_t mask = 0;
    if (n > 0) {
        const uint32_t voices = m.U32(kSoundSystem + 0x0C);
        for (int32_t i = 0; i < n; ++i) {
            const uint32_t h = m.U32(handles + 4u * U(i));
            const uint32_t ch = h >> 27;
            if ((m.U32(voices + 44u * ch + 4u) & 0x07FFFFFFu) == (h & 0x07FFFFFFu)) mask |= Sllv(1u, ch);
            if (m.Faulted()) return 0;
        }
    }
    m.W32(kSoundSystem + 0x14, m.U32(kSoundSystem + 0x14) | mask);          // KeyOn 0x8001EB28
    return m.U32(kSoundSystem + 0x14);
}

// ============================================================================ the frame
void AudioFrame(SoundMachine& s, uint32_t sp) {
    GuestRam& m = s.m;
    const uint32_t gp = m.gp();
    const uint32_t fr = sp - 64u; // the sp every callee is called at
    bool reverbChanged = false;
    if (m.U32(gp + kEngineRecGp) != 0) {
        if (m.U32(gp + kListenerGp) == 0) goto tail;
        for (uint32_t p = 0; p < m.U32(GameState(s) + 0x30); ++p) {
            const uint32_t view = kViewRecords + kViewRecordBytes * p;
            const uint32_t bike = m.U32(m.U32(gp + kEngineRecGp) + kEngineRecBytes * p);
            {
                const uint32_t gs = GameState(s);
                if (m.S32(view + 0x228) == 2 && (m.U8(gs + 4) & 1u)) m.W32(0x800D6C04u, 0);
            }
            int32_t x, z, vx, vz;
            if (m.U32(view + 0x304) == 0 && m.U32(m.U32(bike + 0x354) + 0x25C) < 3u) {
                vz = m.S32(bike + 0x1D0);
                x = m.S32(bike + 0xB8);
                z = m.S32(bike + 0xC0);
                vx = m.S32(bike + 0x1C8);
            } else {
                const int32_t sp4 = m.S32(view + 0x1E0) >> 4;
                vz = MultLo(m.S16(view + 0x1C6), sp4) >> 8;
                vx = MultLo(m.S16(view + 0x1C2), sp4) >> 8;
                x = m.S32(view + 0xB8);
                z = m.S32(view + 0xC0);
            }
            const int32_t yaw = m.S16(m.U32(kCameraPtr) + 0x7C);
            SetListener(S(p), x, z, vx, vz, yaw, ListenerView(s));          // 0x80019100
            {
                const uint32_t rider = m.U32(bike + 0x354);
                const uint32_t flags = (m.U32(rider + 0x25C) < 3u) ? m.U32(bike + 0x24) : m.U32(rider + 0x24);
                const uint32_t target = (((flags >> 5) & 3u) == 1u) ? 2u : 0u;
                const uint32_t L = m.U32(gp + kListenerGp) + kListenerBytes * p;
                if (target != m.U32(L + 0x18) && m.U32(gp + kRoadMuteGp) == 0) {
                    m.W32(L + 0x18, 1);
                    const uint32_t depth = target ? 0x4FFFu : 0u;
                    reverbChanged = true;
                    const int32_t last = m.S32(L + 0x1C);
                    int32_t step;
                    if (last < 0) step = 0x800;
                    else step = S(U(AbsW(S(U(last) - U(m.S16(bike + 0x172))))) << 11);
                    if (target == 0) step = -step;
                    const uint32_t L2 = m.U32(gp + kListenerGp) + kListenerBytes * p;
                    const uint32_t ramp = m.U32(L2 + 0x20) + U(step);
                    m.W32(L2 + 0x20, ramp);
                    m.W32(L2 + 0x1C, U(m.S16(bike + 0x172)));
                    if (!(ramp < 0x5000u)) {
                        m.W32(L2 + 0x18, target);
                        m.W32(L2 + 0x20, depth);
                        m.W32(L2 + 0x1C, 0xFFFFFFFFu);
                    }
                }
            }
            EngineNote(s, S(p), fr);                                         // 0x80019204
            RoadNote(s, S(p));                                               // 0x8001920C
            if (s.Faulted()) return;
            for (uint32_t k = 0; k < 2; ++k) {                               // the delayed pair
                const uint32_t q = m.U32(gp + kListenerGp) + kListenerBytes * p + 4u * k;
                if (m.U32(q + 0x38) == 0) continue;
                const int32_t left = m.S32(q + 0x40) - 1;
                m.W32(q + 0x40, U(left));
                if (left > 0) continue;
                const int32_t id = m.S32(q + 0x30);
                const uint32_t ent = m.U32(q + 0x38);
                m.W32(q + 0x40, 0);
                GuestPlaySound3D(s, m.S32(ent + 0xB8), m.S32(ent + 0xC0), id, 0); // 0x80019264
                const uint32_t q2 = m.U32(gp + kListenerGp) + kListenerBytes * p + 4u * k;
                m.W32(q2 + 0x38, 0);
                m.W32(q2 + 0x30, 0);
            }
        }
        if (reverbChanged) {
            const uint32_t L = m.U32(gp + kListenerGp);
            if (m.U32(GameState(s) + 0x30) == 1) SetReverbDepth(s, m.U32(L + 0x20), m.U32(L + 0x20));
            else SetReverbDepth(s, m.U32(L + 0x20), m.U32(L + 0x68));
        }
    }
    if (m.U32(gp + kListenerGp) == 0) goto tail;
    {
        const uint32_t n = m.U32(gp + kFrameCounterGp);
        m.W32(gp + kFrameCounterGp, n + 1u);
        if ((n & 3u) == 0) {
            const int32_t who = S((n + 1u) & 4u) >> 2;
            if (who == 0) ObjSoundPick(s, 0, fr);
            else if (m.U32(GameState(s) + 0x30) == 2) ObjSoundPick(s, who, fr);
            if (s.Faulted()) return;
        }
    }
    for (uint32_t p = 0; p < m.U32(GameState(s) + 0x30); ++p) {
        {
            const uint32_t L = m.U32(gp + kListenerGp) + kListenerBytes * p;
            if (!(S(m.U32(L + 0x24) + m.U32(L + 0x28)) > 0)) continue;
        }
        for (int32_t slot = 0;; ++slot) {
            const uint32_t e = m.U32(gp + kObjSlotsGp + 4u * p) + kObjSlotBytes * U(slot);
            const uint32_t id = m.U16(e);
            if (id != 0xE0) {
                uint32_t ent = EntityOf(s, id);
                bool riderSelf = false;
                if (m.S32(e + 0x08) == 2 && !(m.U32(m.U32(ent + 0x354) + 0x25C) < 3u)) {
                    ent = m.U32(ent + 0x354);
                    riderSelf = true;
                }
                m.W32(e + 0x0C, m.U32(ent + 0xB8));
                m.W32(e + 0x10, m.U32(ent + 0xC0));
                const uint32_t id2 = m.U16(e);
                uint32_t vAt, spAt;
                if ((id2 >> 5) == 5 && !((id2 & 31u) < 29)) {
                    vAt = ent + 0xD6;
                    spAt = ent + 0xDC;
                    m.W32(e + 0x08, 6);
                } else {
                    vAt = ent + 0x1C2;
                    spAt = ent + 0x1E0;
                }
                const int32_t sp4 = m.S32(spAt) >> 4;
                m.W32(e + 0x14, U(MultLo(m.S16(vAt), sp4) >> 8));
                m.W32(e + 0x18, U(MultLo(m.S16(vAt + 4), sp4) >> 8));
                int32_t kind = m.S32(e + 0x08);
                if (kind == 1) {
                    int32_t pitch;
                    if ((m.U16(ent + 0xAC) >> 5) == 0) {
                        const int32_t r = m.S32(ent + 0x25C);
                        pitch = (r >> 16) + (r >> 17);
                    } else {
                        pitch = m.S32(ent + 0x1E0) >> 10;
                    }
                    pitch += 384;
                    if (pitch < 704) pitch = 704;
                    if (m.U32(e + 0x04) == 0) {
                        int32_t snd;
                        if ((m.U16(ent + 0xAC) >> 5) == 0) {
                            const uint32_t b4 = m.U32(ent + 0xB4);
                            if (b4 < 9u) snd = S((m.U8(m.U32(ent + 0x43C) + 1) >> 4) & 1u);
                            else if (b4 < 18u) snd = S(((m.U8(m.U32(ent + 0x43C) + 1) >> 4) & 1u) + 2u);
                            else snd = 4;
                        } else {
                            snd = m.S8(kObjSoundTable + m.U32(ent + 0xB4) % 6u);
                        }
                        m.W32(e + 0x1C, U(pitch));
                        ObjSoundStart(s, slot, S(p), m.S32(gp + kRoadBankGp), snd, fr); // 0x80019620
                        m.W32(e + 0x08, 1);
                    } else {
                        m.W32(e + 0x1C, U(pitch));
                    }
                    const uint32_t b4 = m.U32(ent + 0xB4);
                    bool claim = false;
                    if (!(b4 < 18u) && m.U32(m.U32(ent + 0x354) + 0x25C) < 3u) claim = true;
                    else if ((m.U8(GameState(s) + 4) & 1u) && (m.U16(ent + 0xAC) >> 5) == 3u && b4 == 0) claim = true;
                    if (claim) ObjSoundClaim(s, m.U16(e), S(p), fr);           // 0x800196A8
                }
                kind = m.S32(e + 0x08);                                       // 0x800196B0
                if (kind == 6 && m.U32(e + 0x04) == 0 && m.S32(gp + kSirenSoundGp) >= 0) {
                    const uint32_t pitch = GetSoundPitch(s, m.S32(gp + kRoadBankGp), m.S32(gp + kSirenSoundGp), 0);
                    m.W32(e + 0x1C, pitch);
                    ObjSoundStart(s, slot, S(p), m.S32(gp + kRoadBankGp), m.S32(gp + kSirenSoundGp), fr); // 0x800196FC
                    kind = m.S32(e + 0x08);
                }
                if (kind == 2 && m.S32(e + 0x24) < m.S32(GameState(s) + 0x0C)) {
                    const uint32_t who = riderSelf ? ent : m.U32(ent + 0x354);
                    m.W8(who + 0x23C, static_cast<uint8_t>(m.U8(who + 0x23C) & 0xF3u));
                    ObjSoundStop(s, slot, S(p));                              // 0x80019770
                }
                if (U(m.S32(e + 0x08) - 3) < 3u && !(m.U32(ent + 0xB4) < 18u) &&
                    !(m.U32(m.U32(ent + 0x354) + 0x25C) < 3u))
                    ObjSoundStop(s, slot, S(p));                              // 0x800197C0
                if (m.U16(e) != 0xE0 && m.U32(e + 0x04) != 0) ObjSoundUpdate(s, slot, S(p), fr); // 0x800197E8
                if (s.Faulted()) return;
            }
            const uint32_t L = m.U32(gp + kListenerGp) + kListenerBytes * p;
            if (!(slot + 1 < S(m.U32(L + 0x24) + m.U32(L + 0x28)))) break;
        }
    }
tail:
    {
        const uint32_t h = m.U32(kCueHandle);
        if (h != 0) {
            const uint32_t gs = GameState(s);
            if (m.S32(kCueHandle + 0x04) < m.S32(gs + 0x10)) {
                StopVoice(s, h);                                             // 0x80019874
                const int32_t cue = m.S32(kCueHandle + 0x0C);
                m.W32(kCueHandle, 0);
                if (cue >= 0) {
                    m.W32(kCueHandle + 0x08, 1);
                    m.W32(kCueSlots + (U(cue) << 5) + 0x08, 4);
                }
            }
        }
        if (m.U32(kCueHandle + 0x08) == 0 && m.S32(0x8005AD48u) == 8) m.W32(kCueHandle + 0x08, 1);
        if (m.U32(kCueHandle + 0x10) != 0 && m.S32(kCueHandle + 0x14) < m.S32(GameState(s) + 0x10)) {
            m.W32(kCueHandle + 0x10, 0);
            m.W32(kCueHandle + 0x14, 0);
        }
        if (m.U32(gp + kPendingCueGp) != 0 && m.S32(kCueSlots + 0x08) == 2) {
            GuestPlaySound3D(s, 0, 0, 0, m.S32(kCueSlots));                   // 0x80019954
            m.W32(gp + kPendingCueGp, 0);
        }
    }
}

// ---------------------------------------------------------------------------- the animation's sounds
uint32_t AnimSoundFire(SoundMachine& s, int32_t x, int32_t z, uint32_t rec, uint32_t flags) {
    GuestRam& m = s.m;
    const uint32_t w = m.U32(rec + 4u);                              // s0
    bool fire = false;
    if (w & 0x01000000u) fire = true;                                // 0x80017DDC
    else if ((w & 0x04000000u) && (flags & 0x80u)) fire = true;      // 0x80017DF4
    else if ((w & 0x02000000u) && !(flags & 0x80u)) fire = true;     // 0x80017E0C
    else if ((w & 0x40000000u) && ((flags >> 4) & 3u) != 2u) fire = true; // 0x80017E2C
    else if ((w & 0x20000000u) && ((flags >> 4) & 3u) == 0u && !(flags & 0x40u)) {
        const uint32_t rc = GetRCnt(s, 0xF2000002u) & 0xFFu;        // 0x80017E5C
        if (S(rc * 9u) >> 8 == 0) fire = true;
    }
    if (fire) {
        uint32_t id = m.U16(rec + 4u);                               // 0x80017E88
        const uint32_t kind = w & 0xFF000000u;
        if (kind == 0x14000000u) {
            id = (flags & 0xFu) + 61u;                               // 0x80017EC8
        } else if (kind > 0x14000000u) {
            if (kind == 0x40000000u) {                               // 0x80017F00
                const uint32_t a0 = (GetRCnt(s, 0xF2000002u) & 0xFFu) >> 7;
                id = ((flags >> 4) & 3u) == 1u ? a0 + 92u : a0 + 90u;
            }
        } else if (kind == 0x09000000u) {                            // 0x80017ED4
            const uint32_t rc = GetRCnt(s, 0xF2000002u) & 0xFFu;
            id += U(MultLo(S((w >> 16) & 0xFFu), S(rc)) >> 8);
        }
        GuestPlaySound3D(s, x, z, S(id), 0);                         // 0x80017F34
    }
    return m.U32(rec + 8u);                                          // 0x80017F3C
}

void AnimSounds(SoundMachine& s, uint32_t desc) {
    GuestRam& m = s.m;
    if (m.U32(desc + 8u) == 0) return;                               // 0x80018E78
    if (m.S32(desc + 12u) <= 0) return;                              // 0x80018E88
    uint32_t off = 0;
    for (int32_t i = 0; i < m.S32(desc + 12u); ++i, off += 2108u) { // 0x80018F7C: the count re-read
        const uint32_t o = m.U32(desc) + off;                        // 0x80018E94: the array re-read
        if (!(m.U32(o + 36u) & 2u)) continue;                        // playing
        if (m.U32(o + 0x6DCu) == 0) continue;
        for (uint32_t guard = 0;; ++guard) {                         // 0x80018EC4
            if (s.Faulted()) return;
            if (guard == 65536u) {
                s.Fail("AnimSounds: an event list that does not end (the original loops forever)", o + 0x6DCu);
                return;
            }
            const uint32_t rec = m.U32(o + 0x6DCu);
            if (m.U32(o + 16u) < m.U32(rec)) break;                  // the next event is still ahead
            const uint32_t struck = m.U8(o + 36u) & 0x80u;
            uint32_t at = m.U32(o);                                  // the owner
            uint32_t flags = struck;
            if ((m.U16(at + 0xACu) >> 5) == 1u && m.U32(at + 0x25Cu) < 3u) {   // a rider near his bike
                const uint32_t bike = m.U32(at + 0x254u);
                const uint32_t players = m.U32(m.U32(kSoundGameStatePtr) + 0x30u);
                const uint32_t handle = m.U16(bike + 0xACu);
                const uint32_t def = m.U32(bike + 0x43Cu);
                flags = struck | (m.U8(def + 46u) & 0xFu) | ((m.U8(def + 1u) & 3u) << 4);
                if (handle < players) flags |= 0x40u;
                at = bike;
            }
            const uint32_t next = AnimSoundFire(s, m.S32(at + 184u), m.S32(at + 192u), m.U32(o + 0x6DCu), flags & 0xFFu);
            m.W32(o + 0x6DCu, next);                                 // 0x80018F78
            if (next == 0) break;
        }
    }
}

} // namespace rr::sim
