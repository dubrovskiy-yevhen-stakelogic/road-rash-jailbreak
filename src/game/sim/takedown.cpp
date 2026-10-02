// See takedown.h. Every load and store in the original's order.
#include "game/sim/takedown.h"

#include "game/sim/fight.h"

namespace rr::sim {

namespace {
constexpr uint32_t kGsPtr = 0x8005B2F8;
constexpr uint32_t kDashBase = 0x800D6198; // s4 at 0x800BF538; the store is +140 (= fight::kDashFlash)
constexpr uint32_t kBustedStream = 0x800D6858;  // = kSpeechFile
constexpr uint32_t kBustedCategory = 0x800D6C40; // = kSpeechCategories[0]
constexpr uint32_t kBustedSlotState = 0x800D6AA8; // = kSpeechSlots[0] + 8
constexpr uint32_t kPendingCue = 1944;           // gp+1944 (sound_frame.h kPendingCueGp)
} // namespace

int Takedown(GuestRam& g, uint32_t B) { // RASHCDG 0x800BF51C
    int credited = 0;
    uint32_t e = fight::kLastBlow;
    for (int i = 0; i < 4; ++i, e += 12u) {
        if (g.U32(e + 4u) != B) continue;                                     // 0x800BF55C
        const uint32_t gs = g.U32(kGsPtr);
        const uint32_t when = g.U32(e + 8u);
        if (!(static_cast<int32_t>(g.U32(gs + 16u) - when) < 240)) continue;  // slti 240
        fight::FightStat(g, g.U32(e), 1, 0);                                  // 0x800BF588
        fight::FightStat(g, B, 1, 1);                                         // 0x800BF598
        const uint32_t h = g.U16(g.U32(e) + 172u);
        const uint32_t d = 1u - h;
        const uint32_t idx = h + ((static_cast<uint32_t>(static_cast<int32_t>(d) >> 31)) & d);
        g.W32(kDashBase + idx * 224u + 140u, 1);                              // 0x800BF5D0
        ++credited;
    }
    return credited;
}

uint32_t RiderOffSound(SoundMachine& s, uint32_t h, uint32_t mode) { // SLUS 0x80018440
    GuestRam& g = s.m;
    const uint32_t off = ((h << 5) + h) << 2;
    const uint32_t arr = g.U32(kSoundGp + kEngineRecGp);
    if (arr == 0) return off;
    const uint32_t E = arr + off;
    if (mode != 0) {
        g.W32(E + 124u, 7);
        return 7;
    }
    StopVoice(s, g.U32(E + 36u));                                              // 0x80018478
    StopVoice(s, g.U32(E + 40u));                                              // 0x80018484
    g.W32(E + 84u, 0xFFFFFFFFu);
    g.W32(E + 88u, 0xFFFFFFFFu);
    g.W32(E + 124u, 0);
    return 0xFFFFFFFFu;
}

uint32_t BustedMusic(SoundMachine& s, SpeechCallees& c, bool& ok) { // SLUS 0x8001B3C8
    GuestRam& g = s.m;
    ok = true;
    const uint32_t gs = g.U32(kGsPtr);
    if (!(g.U32(gs + 48u) < 2u)) return 0;                                     // sltiu
    if (g.U8(gs + 4u) & 1u) return 1;
    g.W32(kBustedCategory, 7);
    g.W32(kBustedStream + 4u, 0x230000u);
    uint32_t v0 = 0;
    ok = c.StreamRequest(kBustedStream, 1, 1, 0, v0);                           // 0x8001B420
    if (!ok) return 0;
    g.W32(kSoundGp + kPendingCue, 1);
    g.W32(kBustedSlotState, 4);
    return 4;
}

} // namespace rr::sim
