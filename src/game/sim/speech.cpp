// The riders' voices - see speech.h. Transcribed from our own disassembly of SLUS_010.53
// (0x8001A0C0..0x8001B244, 0x80016464, 0x8001EA54, 0x8001ED28..0x8001EE94); every load, store and
// call in the original's order where a store can alias a later load (the slot table, the listener
// records and the object slots are re-read after every callee, as the original re-reads them).
#include "game/sim/speech.h"

#include "game/sim/ai.h"
#include "game/sim/fight.h"

namespace rr::sim {
namespace {

int32_t S(uint32_t v) { return static_cast<int32_t>(v); }
uint32_t U(int32_t v) { return static_cast<uint32_t>(v); }
int32_t AbsW(int32_t x) { // `sra 31 / addu / xor`
    const int32_t m = x >> 31;
    return S((U(x) + U(m)) ^ U(m));
}
uint32_t Slot(uint32_t i) { return kSpeechSlots + kSpeechSlotBytes * i; }
uint32_t Gs(GuestRam& m) { return m.U32(kSoundGameStatePtr); }
uint32_t Players(GuestRam& m) { return m.U32(Gs(m) + 48u); }
constexpr uint32_t kPool0 = 0x8005B3A0;
constexpr uint32_t kBankCountAt = kSoundSystem + 0x00, kBankTableAt = kSoundSystem + 0x04;

} // namespace

// ============================================================================ the banks
int32_t LoadBank(SoundMachine& s, SpeechCallees& c, uint32_t bank, uint32_t src, int32_t slot, uint32_t cb,
                 uint32_t cbArg, uint32_t a5) {
    GuestRam& m = s.m;
    if (slot < 0) {                                                   // 0x8001ED50
        uint32_t p = m.U32(kBankTableAt);
        slot = 0;
        while (m.U32(p) != 0) {
            p += 4u;
            ++slot;
            if (S(m.U32(kBankCountAt)) < slot) return -1;
        }
    }
    const uint32_t at = U(slot) * 4u + m.U32(kBankTableAt);
    if (m.U32(at) != 0) return -2;
    uint32_t v0 = 0;
    if (!c.UploadSamples(bank, src, cb, cbArg, a5, v0)) {
        s.Fail("UploadSamples SLUS 0x8001E938 was not served", bank);
        return -3;
    }
    if (S(v0) <= 0) return -3;
    m.W32(bank + 8u, v0);                                             // the jal's delay slot
    PatchBank(s, bank, v0);
    m.W32(U(slot) * 4u + m.U32(kBankTableAt), bank);
    return slot;
}

void UnloadBank(SoundMachine& s, SpeechCallees& c, int32_t slot) {
    GuestRam& m = s.m;
    const uint32_t b = m.U32(U(slot) * 4u + m.U32(kBankTableAt));
    if (b == 0) return;
    if (!c.SpuFree(m.U32(b + 8u))) s.Fail("SpuFree SLUS 0x8004F998 was not served", b);
    m.W32(b + 8u, 0xFFFFFFFFu);
}

void BankFree(SoundMachine& s, SpeechCallees& c, int32_t slot) {
    GuestRam& m = s.m;
    if (S(m.U32(kBankCountAt)) < slot) return;
    if (slot < 0) return;
    if (m.U32(kBankTableAt) == 0) return;
    UnloadBank(s, c, slot);
    m.W32(U(slot) * 4u + m.U32(kBankTableAt), 0);
}

// ============================================================================ the slots
int32_t SpeechSlotFind(SoundMachine& s, SpeechCallees& c, uint32_t id) {
    GuestRam& m = s.m;
    const uint32_t gp = m.gp();
    uint32_t band = 2;                                                // s0
    const uint32_t E = m.U32(gp + kEngineRecGp);
    if (E != 0) {
        const uint32_t bike = m.U32(E);
        if (bike != 0) {
            uint32_t place = 0;
            if (!c.ComputePlace(bike, 0, place)) {
                s.Fail("ComputePlace SLUS 0x800138E8 was not served", bike);
                return -1;
            }
            if (place < 6u) band = 1;
        }
    }
    int32_t row = S((GetRCnt(s, 0xF2000002u) & 0xFFu) * 9u) >> 8;     // a2
    const uint32_t want = id & 0xFFu, speaker = want >> 4;
    for (int32_t k = 0; k < 9; ++k) {
        const uint32_t cat = kSpeechCategories + 4u * U(row);
        const uint32_t a1 = m.U32(cat);
        if ((a1 & 0xFu) < 2u) m.W32(cat, (a1 & 3u) | (band << 4));
        if (m.U32(cat) == want) {
            const uint32_t t = Slot(U(row));
            if (speaker != static_cast<uint32_t>(m.U8(t + 4u) >> 4u)) return row;
            const uint32_t state = m.U32(t + 8u);
            if (!(state < 3u) && m.U32(t) != m.U32(gp + kDefaultBankGp)) return row;
            if (state == 0) return row;
        }
        if (++row >= 9) row = 0;
    }
    return -1;
}

uint32_t SpeechBankLoad(SoundMachine& s, SpeechCallees& c, uint32_t rec, uint32_t id, uint32_t buffer) {
    GuestRam& m = s.m;
    const uint32_t gp = m.gp();
    const int32_t i = SpeechSlotFind(s, c, id);
    if (s.Faulted()) return 0;
    if (i < 0) return 2;
    const uint32_t t = Slot(U(i));
    if (m.U32(t + 8u) == 3u) {
        const uint32_t b = id & 0xFFu;
        if (b != 6u && b != 22u) return 2;
        // a player's own line replaces the one playing: its cue slots are stopped (0x8001A148)
        for (uint32_t p = 0; p < Players(m); ++p) {
            const uint32_t L = m.U32(gp + kListenerGp) + kListenerBytes * p;
            const int32_t first = m.S32(L + 36u);
            uint32_t e = m.U32(gp + kObjSlotsGp + 4u * p) + kObjSlotBytes * U(first);
            const int32_t end = first + m.S32(L + 40u);
            for (int32_t k = first; k < end; ++k, e += kObjSlotBytes) {
                if (m.U16(e) < Players(m) && m.U32(e + 8u) == 2u) ObjSoundStop(s, k, S(p));
                if (s.Faulted()) return 0;
            }
        }
    }
    const int32_t old = m.S32(t);
    m.W32(t + 8u, 1);
    if (old >= 0) BankFree(s, c, old);
    GuestCopyWords(m, m.U32(t + 28u), rec + 8u, 56);
    const uint32_t arg = (U(i) << 16) | (buffer | 0x80000000u);
    const int32_t slot = LoadBank(s, c, m.U32(t + 28u), rec + 64u, m.S32(t), kSpeechUploadCb, arg, 1);
    m.W32(t + 0u, U(slot));
    m.W32(t + 4u, id);
    m.W32(t + 12u, m.U32(rec));
    const uint32_t second = m.U32(rec + 4u);
    m.W32(t + 20u, 0);
    m.W32(t + 24u, 1);
    m.W32(t + 16u, second);
    return 1;
}

void SpeechUploaded(SoundMachine& s, SpeechCallees& c, uint32_t arg) {
    GuestRam& m = s.m;
    if (arg == 0) {
        m.W32(m.gp() + 1876u, 1);
        return;
    }
    m.W32(kSpeechSlots + ((arg >> 11) & 0xFE0u) + 8u, 2);
    if (!c.ReleaseBuffer(arg & 0xFFu)) s.Fail("the stream buffer release SLUS 0x80030FA0 was not served", arg);
}

// ============================================================================ the race's set-up
void SpeechInit(SoundMachine& s, SpeechCallees& c) {
    GuestRam& m = s.m;
    const uint32_t gp = m.gp();
    const uint32_t cat = kSpeechCategories;
    const uint32_t gs = Gs(m);
    m.W32(cat + 0u, 6);
    m.W32(cat + 16u, 3);
    m.W32(cat + 20u, 11);
    m.W32(cat + 4u, 0);
    m.W32(cat + 8u, 1);
    m.W32(cat + 12u, 4);
    m.W32(cat + 24u, 4);
    m.W32(cat + 28u, 255);
    m.W32(cat + 32u, 15);
    if (m.U32(gs + 48u) == 2u) {                                      // two players: the second's own lines
        m.W32(cat + 28u, 22);
        m.W32(cat + 32u, 7);
    }
    if (m.U8(gs + 4u) == 33u) {
        m.W32(cat + 4u, 7);
        m.W32(cat + 8u, 7);
        m.W32(cat + 12u, 7);
        m.W32(cat + 24u, 7);
    }
    const uint32_t kind = m.U8(gs + 4u);
    if ((kind & 1u) && kind != 33u) {                                 // 0x8001A4E0
        m.W32(cat + 24u, 23);
        if (m.U32(gs + 48u) == 1u) m.W32(cat + 0u, 7);
        if (m.S32(0x8005B1F8u) < 3 || !(m.U32(m.U32(kPool0) + 2372u) < 9u)) m.W32(cat + 4u, 255);
        else m.W32(cat + 8u, 255);
        if (m.U32(m.U32(kSoundGameStatePtr) + 48u) == 1u) m.W32(cat + 12u, 255);
    }
    for (uint32_t i = 0; i < 9; ++i) {                                // 0x8001A580
        if (!(m.U32(cat + 4u * i) & 8u)) {
            uint32_t block = 0;
            if (!c.Malloc(56, 0, block)) {
                s.Fail("Malloc SLUS 0x8001447C was not served", 56);
                return;
            }
            m.W32(Slot(i) + 28u, block);
        }
        m.W32(Slot(i) + 12u, 255);
        m.W32(Slot(i) + 16u, 255);
    }
    if (m.S32(kSpeechFile) < 0) return;                               // 0x8001A5CC
    if (m.U8(m.U32(kSoundGameStatePtr) + 4u) == 33u) {
        m.W32(kSpeechFile + 4u, 0x23C000u);
    } else {
        const uint32_t r = GetRCnt(s, 0xF2000002u) & 0xFFu;
        m.W32(kSpeechFile + 4u, U(S(r * 133u) >> 8) << 14);           // one of records 0..132
    }
    uint32_t v0 = 0;
    if (!c.StreamRequest(kSpeechFile, 7, 0, 0, v0)) {
        s.Fail("StreamRequest SLUS 0x80023148 was not served", kSpeechFile);
        return;
    }
    const uint32_t k2 = m.U8(m.U32(kSoundGameStatePtr) + 4u);
    if ((k2 & 1u) && k2 != 33u) {
        m.W32(kSpeechFile + 4u, m.U32(cat + 8u) == 255u ? 0x234000u : 0x238000u);
        if (!c.StreamRequest(kSpeechFile, 1, 0, 0, v0)) {
            s.Fail("StreamRequest SLUS 0x80023148 was not served", kSpeechFile);
            return;
        }
    }
    m.W32(kCueHandle + 0u, 0);                                        // 0x8001A6A4: the cue voice
    m.W32(kCueHandle + 4u, 0);
    m.W32(kCueHandle + 8u, 1);
    m.W32(kCueHandle + 16u, 0);
    m.W32(kCueHandle + 20u, 0);
    const uint32_t def = m.U32(gp + kDefaultBankGp);
    const uint32_t t5 = Slot(5);                                      // the crash exclamations 88 / 89
    m.W32(t5 + 4u, 11);
    m.W32(t5 + 12u, 11);
    m.W32(t5 + 16u, 11);
    m.W32(t5 + 20u, 88);
    m.W32(t5 + 24u, 89);
    m.W32(gp + kPendingCueGp, 0);
    m.W32(t5 + 8u, 2);
    m.W32(t5 + 28u, 0);
    m.W32(t5 + 0u, def);
    const uint32_t c8 = m.U32(cat + 32u);
    if (c8 == 15u) {                                                  // the default bank's sound 108
        const uint32_t t8 = Slot(8);
        m.W32(t8 + 4u, 7);
        m.W32(t8 + 16u, 255);
        m.W32(t8 + 0u, def);
        m.W32(t8 + 8u, 2);
        m.W32(t8 + 12u, c8);
        m.W32(t8 + 20u, 108);
        m.W32(t8 + 24u, 108);
        m.W32(t8 + 28u, 0);
    }
    m.W32(gp + kSpeechToggleGp, 0);
}

// ============================================================================ the line
void RiderSpeech(SoundMachine& s, SpeechCallees& c, uint32_t h, int32_t crash, uint32_t sp) {
    GuestRam& m = s.m;
    const uint32_t gp = m.gp();
    const uint32_t fsp = sp - 128u;
    m.W32(sp + 0u, h);                                                // the caller's home slots
    m.W32(sp + 4u, U(crash));
    uint32_t cand = 255, row = 0, col = 0;                            // s8, s4, s6
    uint32_t playerOrCop = 0, oneShot = 0, prevBank = 0, prevSnd = 0, reuse = 0; // sp+44/48/52/56/64
    const uint32_t S0 = m.U32(kPool0) + 1096u * h;                    // s1
    if (Players(m) == 0) return;
    uint32_t lOff = 0, pOff = 0, p = 0;                               // sp+76, sp+80, sp+60
    auto isPlayer = [&m, S0]() { return m.U16(S0 + 172u) < Players(m); };
    for (;;) {
        if (m.U32(gp + kObjSlotsGp + pOff) == 0) return;
        uint32_t s5 = m.U32(0x8005B268u + pOff);                      // this listener's bike
        bool next = false;
        if (crash == 0) {
            if (0xF000 < AbsW(S(m.U32(s5 + 324u) - m.U32(S0 + 324u)))) next = true;
            else if (!isPlayer() && m.U32(m.U32(gp + kListenerGp) + lOff + 44u) != 0 && m.U32(S0 + 1088u) != 0)
                next = true;                                          // this listener hears an AI's taunt
        }
        if (!next) {
            // ---- 0x8001A8A0: the speaker's line already playing on this listener stops
            const uint32_t L = m.U32(gp + kListenerGp) + lOff;
            const uint32_t first = m.U32(L + 36u);
            uint32_t e = m.U32(gp + kObjSlotsGp + pOff) + kObjSlotBytes * first;
            const uint32_t end = first + m.U32(L + 40u);              // sp+40
            for (uint32_t k = first; k < end; ++k, e += kObjSlotBytes) {
                if (m.U16(e) == h && m.S32(e + 8u) < 3) {
                    if (crash == 0 && !isPlayer() && m.U32(S0 + 1088u) != 0) return;
                    ObjSoundStop(s, S(k), S(p));
                    break;
                }
            }
            if (s.Faulted()) return;
            uint32_t bank = 0, snd = 0;                               // sp+68, sp+72
            if (reuse) {                                              // 0x8001A998: the first listener's pick
                bank = prevBank;
                snd = prevSnd;
            } else {
                // ---- the candidate categories
                uint32_t cands[3] = {255, 255, 255};                  // sp+16..24
                if (crash != 0) {
                    cands[0] = 3;
                    cands[1] = 11;
                    cands[2] = 255;
                } else if (isPlayer()) {
                    const bool kind2 = (m.U8(m.U32(S0 + 1084u) + 1u) & 0xFu) == 2u;
                    cands[0] = (kind2 && m.U8(Gs(m) + 4u) != 33u) ? 7u : ((h << 4) | 6u);
                    cands[1] = 255;
                    playerOrCop = 1;
                } else {
                    const uint32_t rd = m.U32(S0 + 1084u);
                    const uint32_t b = m.U8(rd + 1u);
                    const uint32_t who = b >> 4;
                    if ((b & 0xFu) == 2u || m.U8(Gs(m) + 4u) == 33u) {
                        cands[0] = 7;
                        cands[1] = 15;
                        cands[2] = 255;
                        playerOrCop = 1;
                    } else if (who - 1u < 2u || who == 5u) {          // a character with lines of his own
                        cands[0] = (Players(m) == 2u) ? (b & 3u) : m.U8(rd + 1u);
                        cands[1] = (m.U8(m.U32(S0 + 1084u) + 1u) & 3u) + 4u;
                        cands[2] = (m.U8(m.U32(S0 + 1084u) + 1u) & 3u) + 68u;
                    } else {
                        cands[0] = (b & 3u) + 4u;
                        cands[2] = 255;
                        cands[1] = (m.U8(m.U32(S0 + 1084u) + 1u) & 3u) + 68u;
                    }
                }
                // ---- 0x8001AB2C: the table search, from a cell the hardware counter picks
                bool found = false;
                for (uint32_t k = 0;;) {
                    const uint32_t v = cands[k];
                    if (v == 255u) break;
                    cand = v;
                    col = (GetRCnt(s, 0xF2000002u) & 0xFFu) >> 7;
                    row = U(S((GetRCnt(s, 0xF2000002u) & 0xFFu) * 9u) >> 8);
                    for (int32_t i = 0; i < 9; ++i) {
                        if (m.U32(Slot(row) + 12u + 4u * col) == cand) { found = true; break; }
                        col ^= 1u;
                        if (m.U32(Slot(row) + 12u + 4u * col) == cand) { found = true; break; }
                        if (++row >= 9u) row = 0;
                    }
                    ++k;
                    if (k >= 3u || found) break;
                }
                if (s.Faulted() || !found) return;
                // 0x8001AC20: category 6 (a player's own line) and a cop player alternate the column
                if (cand == 6u || ((m.U8(m.U32(S0 + 1084u) + 1u) & 0xFu) == 2u && isPlayer())) {
                    col = (m.U32(gp + kSpeechToggleGp) == 0) ? 1u : 0u;
                    m.W32(gp + kSpeechToggleGp, col);
                }
                if (m.U32(S0 + 1088u) != 0 && (cand - 68u < 2u || (cand == 7u && !isPlayer()))) {
                    const int32_t t = m.S32(Gs(m) + 12u) >> 8;        // the rate limit, 21 x 256 ticks
                    if (AbsW(S(U(t) & 0xFFFFu) - m.S16(S0 + 870u)) < 21) return;
                    m.W16(S0 + 870u, static_cast<uint16_t>(t));
                }
                bank = m.U32(Slot(row));
                snd = m.U32(Slot(row) + 20u + 4u * col);
            }
            // ---- 0x8001AD30: a free speech voice among the cue slots
            uint32_t slot = first;                                    // s3 = s7
            e = m.U32(gp + kObjSlotsGp + pOff) + kObjSlotBytes * m.U32(m.U32(gp + kListenerGp) + lOff + 36u);
            bool free = false;
            for (; slot < end; ++slot, e += kObjSlotBytes)
                if (m.U32(e + 4u) == 0) { free = true; break; }
            if (!free) {
                if (crash == 0 && !isPlayer()) next = true;           // an AI's taunt waits for a voice
                else oneShot = 1;
            }
            if (!next) {
                if (crash == 0) {
                    bool lateral = true;
                    if (isPlayer() || m.U32(S0 + 1088u) == 0) {
                        // ---- 0x8001AE20: THE PROVOCATION - the nearest AI turns on the speaker
                        uint32_t s0 = S0;
                        if (m.U32(S0 + 1088u) == 0) s0 = m.U32(S0 + 856u);
                        const uint32_t t = fight::Pick(m, s0 + 172u, 1) & 0xFFFFu;
                        if (t != 224u) {
                            s5 = m.U32(kPool0) + 1096u * t;
                            const int32_t d = S((m.U32(s5 + 324u) - m.U32(s0 + 324u)) << 4);
                            if (!(0x13FFFF < AbsW(d)) && (m.U32(s5 + 560u) & 0x08000000u)) {
                                const uint32_t rd = m.U32(s5 + 1084u);
                                m.W8(rd + m.U8(kGrudgeIndex + m.U16(s0 + 172u)) + 16u, 15); // 0x8001AED4
                                const int32_t depth = m.S8(s5 + 946u);
                                const uint32_t top = m.U16(s5 + U(8 * (depth - 1)) + 956u);
                                if (top - 4u < 13u && fight::CanEngage(m, s5, s0, d) != 0) {
                                    fight::NextMove(m, m.U32(s5 + 1084u));
                                    m.W16(fsp + 32u, 16);
                                    m.W16(fsp + 34u, m.U16(S0 + 172u));
                                    if (!c.PushCommand(fsp + 32u, 2, s5)) {
                                        s.Fail("AiPushCommand RASHCDG 0x800BCA68 was not served", s5);
                                        return;
                                    }
                                }
                            }
                        }
                        if (m.U32(S0 + 1088u) == 0) lateral = false;
                    }
                    if (lateral) {
                        // ---- 0x8001AF50: which side of the speaker the other bike is on
                        int32_t a2 = 0;
                        const uint32_t pieceX = m.U32(s5 + 360u);
                        if (pieceX == m.U32(S0 + 360u) && ((pieceX >> 16) == 0 || m.U32(s5 + 336u) == m.U32(S0 + 336u))) {
                            a2 = S(m.U32(s5 + 344u) - m.U32(S0 + 344u));
                            if (m.S32(S0 + 364u) < 0) a2 = S(0u - U(a2));
                        } else {
                            int32_t pp[3], o[3];
                            int16_t ax[3];
                            for (uint32_t k = 0; k < 3; ++k) {
                                pp[k] = m.S32(s5 + 184u + 4u * k);
                                o[k] = m.S32(S0 + 184u + 4u * k);
                                ax[k] = m.S16(S0 + 432u + 2u * k);
                            }
                            a2 = AiProject(pp, ax, o);                 // RASHCDG 0x800B6AAC
                        }
                        const uint32_t r = m.U32(S0 + 852u);
                        m.W8(r + 572u, static_cast<uint8_t>(m.U8(r + 572u) | (a2 < 0 ? 8u : 4u)));
                    }
                }
                if (s.Faulted()) return;
                // ---- 0x8001AFF8: the cell is used
                const uint32_t pair = cand & 0xFEu;
                const uint32_t cell = Slot(row) + 12u + 4u * col;
                if (pair == 4u || pair == 68u) m.W32(cell, (cand & 1u) + 68u);
                else if (!reuse && !(cand & 8u) && !playerOrCop) m.W32(cell, 0xFFFFFFFFu);
                if (oneShot) {
                    GuestPlaySound3D(s, m.S32(S0 + 184u), m.S32(S0 + 192u), S(snd), S(bank));
                    m.W32(Slot(row) + 8u, 4);
                } else {
                    // 0x8001B088: the line on speech voice `slot`, following the speaker
                    m.W16(e + 0u, static_cast<uint16_t>(h));
                    m.W32(e + 12u, m.U32(S0 + 184u));
                    m.W32(e + 16u, m.U32(S0 + 192u));
                    m.W32(e + 20u, U(S(U(m.S16(S0 + 450u)) * U(m.S32(S0 + 480u) >> 4)) >> 8));
                    m.W32(e + 24u, U(S(U(m.S16(S0 + 454u)) * U(m.S32(S0 + 480u) >> 4)) >> 8));
                    const uint32_t pitch = GetSoundPitch(s, S(bank), S(snd), 0);
                    m.W32(e + 28u, pitch);
                    m.W32(e + 8u, 2);
                    ObjSoundStart(s, S(slot), S(p), S(bank), S(snd), fsp);
                    if (s.Faulted()) return;
                    const uint32_t gs = Gs(m);
                    m.W32(e + 36u, m.U32(gs + 12u) + 500u);
                    if (!(m.U16(S0 + 172u) < m.U32(gs + 48u)) && crash == 0)
                        m.W32(m.U32(gp + kListenerGp) + lOff + 44u, 1);   // an AI's taunt is playing
                    m.W32(Slot(row) + 8u, 3);
                    m.W32(e + 40u, row);
                }
                if (s.Faulted()) return;
                reuse = 1;
                prevBank = bank;
                prevSnd = snd;
            }
        }
        // ---- 0x8001B1D8: the next listener
        lOff += kListenerBytes;
        pOff += 4u;
        ++p;
        if (!(p < Players(m))) return;
    }
}

} // namespace rr::sim
