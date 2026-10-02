// The race loader's sound set-up in the world mode (sound_runtime.h LoaderSetUp): the PORTED SoundSpuAttr
// SLUS 0x8001E8C4 (libspu's main volume), SoundInit SLUS 0x8001E614 and the
// race loader's SoundLoad RASHCDI 0x800627F8 (src\game\sim\loader.h, bench rows sound_spu_attr / sound_init /
// sound_load), run on the race's arena with these callees:
//   * the mallocs: the host's blocks - the ones the rest of the runtime already reaches (the bank table, the voices,
//     the listeners, the engine records, player 0's object slots, the first three bank records 4 KiB apart) and, for
//     what a one-player race of the captures never asks (player 1's slots, a fourth / fifth bank record), the room
//     the resident records leave (0x2210.. of the heap); the 256 KiB transfer buffer is never written here;
//   * the bank reader RASHCDI 0x80062D84 (its CD seek / read and the wait for the DMA callback): the host's -
//     the entry's record is copied into the malloc'd block the loader handed it, and the PORTED LoadBank SLUS
//     0x8001ED28 (speech.h) registers it: the first free slot, the SPU address, PatchBank, the table entry. The
//     upload 0x8001E938 is ours: the samples into the SPU model's RAM from 0x1010 on, as every capture has the
//     resident three; a bank past those (the siren of a police race, player 1's engine) takes the speech heap's
//     first fit (the music rings sit where the captures have them);
//   * the sound resets 0x8001F054 / 0x8001F080 / 0x8001F0D4 and 0x8001EFE8: named, not run (the runtime's own reset
//     precedes); ResetVoiceLists 0x8001FBD4 and SoundRecordsInit RASHCDI 0x80063448: PORTED;
//   * the file open / read / close: the disc image (RASHNZ_E.DAT's directory; AUDTAUNT.STR's size), handle 1 for
//     both as every capture holds; GetRCnt(2): the product's root counter (root_counter.h);
//   * SpuSetCommonAttr SLUS 0x800510B8: the main volume into the SPU model's 0x180 / 0x182 (mode 0: the level);
//     the CD / external volumes and SPUCNT's mix bits are PlayStation housekeeping the mixer has no use for.
// THE SPU HEAP (sim\spu_heap.h, rows spu_*): libspu's allocator PORTED on the arena. At the set-up the boot's
// SpuInitMalloc(12, 0x800D6A38) (SLUS 0x8001EBF0) runs - OURS, named: in place of the boot's call, the front end's
// history not replayed (every race capture holds a fresh heap in the race's order) - with the four globals the
// boot's SpuInit / SpuSetReverb leave (reverb on, its work area from 0xF6F8 * 8, the unit shift 3 / mask 7). Then
// every upload of 0x8001E938 takes SpuMalloc(bank+0x0C), the music rings the PORTED MusicSpuAlloc SLUS 0x800210C4
// (StartMusic), the speech banks SpuMalloc / SpuFree (sound_runtime.cpp Voices). RRJB_SPUHEAP=off: the old
// placement (the banks from 0x1010 on, the rings at 0x434E0 / 0x534E0, the speech banks a first fit from 0x634E0).
#include "game/audio/sound_runtime.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "game/audio/root_counter.h"
#include "game/sim/loader.h"
#include "game/sim/spu_heap.h"
#include "game/sim/sound_engine.h"
#include "game/sim/sound_frame.h"
#include "game/sim/speech.h"

namespace rr::game {

namespace {
namespace s = rr::sim;
constexpr uint32_t kGpS = s::kSoundGp;
constexpr uint32_t kSpuFirst = 0x1010, kSpuMusic = 0x434E0, kSpuSpeech = 0x634E0, kSpuSpeechTop = 0x80000;
constexpr uint32_t kExtraFrom = 0x2210, kExtraTo = 0x3000; // heap offsets the resident records leave free
constexpr uint32_t kSoundSp = 0x801FF000u;                 // OURS: the stack the set-up runs on

bool LoaderOff() {
    const char* v = std::getenv("RRJB_LOADER");
    return v != nullptr && std::strcmp(v, "off") == 0;
}
bool SpuHeapOff() { // the negative control of the SPU heap port
    const char* v = std::getenv("RRJB_SPUHEAP");
    return v != nullptr && std::strcmp(v, "off") == 0;
}
} // namespace

uint32_t SoundRuntime::SpuHeapMalloc(uint32_t bytes) {
    s::GuestRam g(ram_, kGpS);
    const uint32_t at = s::SpuMalloc(g, bytes);                          // PORTED, SLUS 0x8004F3C8
    if (g.Faulted() || static_cast<int32_t>(at) < 0) {
        ++spuMallocFails_;
        return 0xFFFFFFFFu;
    }
    ++spuMallocs_;
    return at;
}

void SoundRuntime::SpuHeapFree(uint32_t address) {
    s::GuestRam g(ram_, kGpS);
    s::SpuFree(g, address);                                              // PORTED, SLUS 0x8004F998
    ++spuFrees_;
}

// The music stream player's set-up 0x80020BEC is not ported: its writes MusicSpuAlloc reads - two streams of
// 8 x 0x2000 bytes, the stream options every capture holds (0x800D754C / 0x800D7580 / 0x800D7584) - are done here.
bool SoundRuntime::SpuHeapMusic(uint32_t ring[2]) {
    W32(s::kMusicStreamCount, 2);
    W32(s::kMusicChunkBytes, 0x2000);
    W32(s::kMusicChunks, 8);
    struct Print final : s::LoaderCallees { // the debug print 0x80044894: not printed
        bool Call(uint32_t, const uint32_t*, int, uint32_t, uint32_t& v0) override {
            v0 = 0;
            return true;
        }
    } print;
    s::GuestRam g(ram_, kGpS);
    uint32_t v0 = 0;
    const bool ok = s::MusicSpuAlloc(g, kSoundSp, print, v0) && !g.Faulted(); // PORTED, SLUS 0x800210C4
    for (uint32_t k = 0; k < 2u; ++k) {
        ring[k] = R32(s::kMusicStreams + 20u * k + 32u);
        if (ok && v0 == 0) ++spuMallocs_;
    }
    if (!ok || v0 != 0) ++spuMallocFails_;
    return ok && v0 == 0;
}

std::string SoundRuntime::SpuHeapLine() const {
    char b[700];
    if (!spuHeapPorted_) {
        std::snprintf(b, sizeof(b), "SpuMalloc NOT run (%s): the old placement - the banks from 0x1010 on, the music rings at "
                      "0x434E0 / 0x534E0, the speech banks a first fit from 0x634E0",
                      SpuHeapOff() ? "RRJB_SPUHEAP=off, the negative control" : "the loader's sound set-up did not run");
        return b;
    }
    const uint32_t tab = R32(s::kSpuMallocTab);
    const int32_t last = static_cast<int32_t>(R32(s::kSpuMallocLast));
    std::string t;
    for (int32_t k = 0; k <= last && k < 16; ++k) {
        char e[40];
        std::snprintf(e, sizeof(e), " {0x%X,0x%X}", R32(tab + 8u * static_cast<uint32_t>(k)),
                      R32(tab + 8u * static_cast<uint32_t>(k) + 4u));
        t += e;
    }
    std::snprintf(b, sizeof(b), "SpuMalloc PORTED (sim\\spu_heap.h, SLUS 0x8004F3C8 / 0x8004F998 / 0x800210C4): %zu block(s) "
                  "placed, %zu refused, %zu freed; table 0x%08X n %d last %d:%s",
                  spuMallocs_, spuMallocFails_, spuFrees_, tab, static_cast<int32_t>(R32(s::kSpuMallocMax)), last, t.c_str());
    return b;
}

bool SoundRuntime::LoaderSound() const { return !LoaderOff(); }

bool SoundRuntime::LoaderSetUp(s::SoundMachine& sm) {
    s::GuestRam& g = sm.m;
    loaderLine_.clear();
    std::string refused;
    // the boot's SpuInitMalloc(12, 0x800D6A38) (SLUS 0x8001EBF0), PORTED, with the globals SpuInit / SpuSetReverb leave
    spuHeapPorted_ = false;
    spuMallocs_ = spuMallocFails_ = spuFrees_ = 0;
    if (!SpuHeapOff()) {
        W32(s::kSpuReverbOn, 1);
        W32(s::kSpuReverbStart, 0xF6F8);
        W32(s::kSpuAddrShift, 3);
        W32(s::kSpuAddrMask, 7);
        spuHeapPorted_ = s::SpuInitMalloc(g, s::kSpuBootBlocks, s::kSpuBootTable) == s::kSpuBootBlocks && !g.Faulted();
    }
    uint32_t extra = heap_ + kExtraFrom;
    uint32_t spuNext = kSpuFirst;
    int bankBlocks = 0, slotBlocks = 0;
    uint32_t dirAt = 0;
    size_t banksLoaded = 0, deferred = 0;

    // The upload of LoadBank (speech.h's callees; only UploadSamples is reached from here).
    struct Upload final : s::SpeechCallees {
        SoundRuntime& r;
        uint32_t& spuNext;
        const Bank* bank = nullptr;
        Upload(SoundRuntime& rt, uint32_t& next) : r(rt), spuNext(next) {}
        bool ComputePlace(uint32_t, int32_t, uint32_t& v0) override { v0 = 0; return false; }
        bool PushCommand(uint32_t, int32_t, uint32_t) override { return false; }
        bool Malloc(uint32_t, uint32_t, uint32_t& v0) override { v0 = 0; return false; }
        bool StreamRequest(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t& v0) override { v0 = 0; return false; }
        bool SpuFree(uint32_t a) override {
            if (r.spuHeapPorted_) r.SpuHeapFree(a);
            return true;
        }
        bool ReleaseBuffer(uint32_t) override { return true; }
        bool UploadSamples(uint32_t bankAt, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t& v0) override {
            v0 = 0xFFFFFFFFu;
            if (bank == nullptr) return true;
            uint32_t n = static_cast<uint32_t>(bank->samples.size());
            uint32_t at = 0;
            if (r.spuHeapPorted_) {                                       // PORTED: SpuMalloc(bank+0x0C), 0x8001E948
                const uint32_t bytes = r.R32(bankAt + 0x0Cu);
                at = r.SpuHeapMalloc(bytes);
                if (static_cast<int32_t>(at) < 0) return true;            // -1, as 0x8001E938 answers
                constexpr uint32_t kTop = static_cast<uint32_t>(rr::audio::SpuVoices::kRamBytes);
                n = std::min(n, bytes);
                if (at + n > kTop) n = at < kTop ? kTop - at : 0u;
            } else if (spuNext + n <= kSpuMusic) {                               // the resident region, as the captures
                at = spuNext;
                spuNext = (spuNext + n + 15u) & ~15u;
            } else {                                                      // OURS: the speech heap's first fit
                at = kSpuSpeech;
                size_t k = 0;
                for (; k < r.spuHeap_.size(); ++k) {
                    if (at + n <= r.spuHeap_[k].address) break;
                    at = r.spuHeap_[k].address + r.spuHeap_[k].bytes;
                }
                if (at + n > kSpuSpeechTop) return true;
                r.spuHeap_.insert(r.spuHeap_.begin() + static_cast<ptrdiff_t>(k), SpuBlock{at, n});
            }
            std::memcpy(r.spu_->Ram().data() + at, bank->samples.data(), n);
            v0 = at;
            return true;
        }
    };
    Upload up(*this, spuNext);

    struct Callees final : s::LoaderCallees {
        std::function<bool(uint32_t, const uint32_t*, int, uint32_t, uint32_t&)> fn;
        bool Call(uint32_t f, const uint32_t* a, int n, uint32_t sp, uint32_t& v0) override { return fn(f, a, n, sp, v0); }
    } c;
    c.fn = [&](uint32_t f, const uint32_t* a, int, uint32_t, uint32_t& v0) -> bool {
        v0 = 0;
        switch (f) {
        case s::kLdMemset: {                                              // SLUS 0x8001E100's word loop
            const uint32_t w = a[1] | (a[1] << 8) | (a[1] << 16) | (a[1] << 24);
            for (uint32_t k = 0; k < a[2]; k += 4) W32(a[0] + k, w);
            return true;
        }
        case s::kLdMalloc: {                                              // the host's blocks
            const uint32_t n = a[0], gs = R32(0x8005B2F8u);
            const uint32_t players = gs != 0 ? R32(gs + 0x30u) : 1u;
            if (n == players * 72u && R32(kGpS + 1920u) == 0) v0 = listenersAt_;
            else if (n == players * 132u && R32(kGpS + 1952u) == 0) v0 = engineAt_;
            else if (n == 352u) v0 = (slotBlocks++ == 0) ? slotsAt_ : 0u;
            else if (n == 0x40000u) v0 = heap_;                           // the transfer buffer: never written here
            else if (bankBlocks < 3 && n <= 0x1000u) v0 = bankAt_ + 0x1000u * static_cast<uint32_t>(bankBlocks++);
            if (v0 == 0 && n != 0) {                                      // the room the resident records leave
                const uint32_t at = (extra + 7u) & ~7u;
                if (at + n <= heap_ + kExtraTo) {
                    v0 = at;
                    extra = at + n;
                }
            }
            if (v0 == 0 && refused.empty()) refused = "a malloc of " + std::to_string(n) + " bytes found no room";
            return true;
        }
        case s::kLdFree:
        case 0x8001460Cu:                                                 // close
            return true;
        case 0x8001FBD4u:                                                 // ResetVoiceLists, PORTED
            s::ResetVoiceLists(sm);
            return true;
        case 0x80063448u:                                                 // SoundRecordsInit, PORTED
            s::SoundRecordsInit(sm, static_cast<int32_t>(a[0]));
            return true;
        case 0x8001458Cu:                                                 // open: handle 1 (every capture's)
            v0 = 1;
            return true;
        case 0x80014780u: {                                               // read: RASHNZ_E.DAT's directory
            dirAt = a[1];
            for (uint32_t k = 0; k < a[2] && k < bankDirectory_.size(); ++k) *At(a[1] + k) = bankDirectory_[k];
            v0 = a[2];
            return true;
        }
        case 0x80043F00u:                                                 // GetRCnt(2)
            v0 = ConsoleRootCounter2();
            return true;
        case 0x800148BCu:                                                 // the size of AUDTAUNT.STR
            v0 = loaderSpeechBytes_;
            return true;
        case 0x80062D84u: {                                               // the bank reader: the host's, LoadBank PORTED
            const uint32_t entry = dirAt != 0 && a[1] >= dirAt ? (a[1] - dirAt) / 12u : 99u;
            const uint32_t dst = R32(a[2]);
            v0 = 0xFFFFFFFFu;
            if (dst == 0 || entry >= banks_.size()) return true;
            const Bank& b = banks_[entry];
            std::memcpy(At(dst), b.record.data(), b.record.size());
            up.bank = &b;
            const int32_t slot = s::LoadBank(sm, up, dst, 0, -1, 0x80016464u, 0, 1);
            up.bank = nullptr;
            v0 = static_cast<uint32_t>(slot);
            if (slot >= 0 && static_cast<size_t>(slot) < bankSlots_.size()) {
                bankSource_[static_cast<size_t>(slot)] = static_cast<int32_t>(entry);
                bankSpuBase_[static_cast<size_t>(slot)] = R32(dst + 8u);
                bankSlots_[static_cast<size_t>(slot)].address = dst;
                bankSlots_[static_cast<size_t>(slot)].data = At(dst);
                bankSlots_[static_cast<size_t>(slot)].size = static_cast<uint32_t>(b.record.size());
                ++banksLoaded;
            }
            return true;
        }
        case s::kLdSpuCommonAttr: {                                       // libspu: the main volume
            const uint32_t mask = R32(a[0]);
            const uint16_t l = static_cast<uint16_t>(R32(a[0] + 4u) & 0xFFFFu), r = static_cast<uint16_t>(R32(a[0] + 4u) >> 16);
            if (mask & 1u) spu_->Write(0x180, static_cast<uint16_t>(l & 0x7FFFu));
            if (mask & 2u) spu_->Write(0x182, static_cast<uint16_t>(r & 0x7FFFu));
            return true;
        }
        default:
            ++deferred;                                                   // the sound resets, 0x8001EFE8, the tear-down
            return true;
        }
    };

    // the boot's part: SoundSpuAttr, then SoundInit with the option list {1, 0x01002001, 4, 12, 5, 24, 0}
    // (SLUS 0x800163EC..0x80016414, on the stack - OURS: where)
    bool ok = s::SoundSpuAttr(g, kSoundSp, c);
    const uint32_t list = kSoundSp - 1024u;
    const uint32_t opts[7] = {1, 0x01002001u, 4, 12, 5, 24, 0};
    for (uint32_t k = 0; k < 7u; ++k) W32(list + 4u * k, opts[k]);
    int32_t v0 = 0;
    // SoundInit's allocator: the bank table, then the voices (the host's blocks)
    uint32_t sysAllocs = 0;
    auto base = c.fn;
    c.fn = [&](uint32_t f, const uint32_t* a, int n, uint32_t sp, uint32_t& r) -> bool {
        if (f == s::kLdMalloc) {
            r = (sysAllocs++ == 0) ? bankTableAt_ : voicesAt_;
            return true;
        }
        return base(f, a, n, sp, r);
    };
    ok = ok && s::SoundInit(g, list, kSoundSp, c, v0) && v0 == 0;
    c.fn = base;
    for (uint32_t k = 0; k < 7u; ++k) W32(list + 4u * k, 0);
    if (!ok) {
        loaderLine_ = "the race loader's sound set-up REFUSED at SoundInit / SoundSpuAttr - the named layout stands";
        return false;
    }
    if (engineMode_) s::AudioReset(sm);                                   // PORTED (the boot's, before the race)
    // the race loader: the three blocks it makes when missing, the records, the banks
    W32(kGpS + 1920u, 0);
    W32(kGpS + 1952u, 0);
    W32(kGpS + 1924u, 0);
    W32(kGpS + 1928u, 0);
    ok = s::SoundLoad(g, kSoundSp, c) && refused.empty() && !sm.Faulted();
    char b[800];
    std::snprintf(b, sizeof(b),
                  "the race loader's sound set-up PORTED (sim\\loader.h): SoundSpuAttr SLUS 0x8001E8C4 (main volume "
                  "0x%04X / 0x%04X), SoundInit SLUS 0x8001E614 (%u banks, %u voices), SoundLoad RASHCDI 0x800627F8: "
                  "%zu bank(s) through the PORTED LoadBank (default %d, road %d, engine %d, cue %d), listener 0x%08X, "
                  "engine records 0x%08X, %zu call(s) named and not run; SpuInitMalloc(12, 0x800D6A38) %s, %zu bank "
                  "block(s) through the PORTED SpuMalloc%s%s",
                  spu_->Read(0x180), spu_->Read(0x182), R32(0x800D6870u), R32(0x800D6878u), banksLoaded,
                  static_cast<int32_t>(R32(kGpS + 1912u)), static_cast<int32_t>(R32(kGpS + 1940u)),
                  static_cast<int32_t>(R32(R32(kGpS + 1952u) + 8u)), static_cast<int32_t>(R32(kGpS + 1904u)),
                  R32(kGpS + 1920u), R32(kGpS + 1952u), deferred,
                  spuHeapPorted_ ? "PORTED" : (SpuHeapOff() ? "not run (RRJB_SPUHEAP=off)" : "REFUSED"), spuMallocs_,
                  ok ? "" : " - REFUSED: ",
                  ok ? "" : (refused.empty() ? "a fault" : refused.c_str()));
    loaderLine_ = b;
    loaderRan_ = ok;
    spuHeapPorted_ = spuHeapPorted_ && ok;
    return ok;
}

} // namespace rr::game
